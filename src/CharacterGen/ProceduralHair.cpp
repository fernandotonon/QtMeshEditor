#include "ProceduralHair.h"

#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <map>

namespace CharacterGen {
namespace Hair {

namespace {

constexpr float kPi = 3.14159265358979f;

float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
float smooth01(float t)
{
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float lerpf(float a, float b, float t) { return a + (b - a) * t; }
float smoothstep(float a, float b, float x) { return smooth01((x - a) / (b - a)); }

// Cheap deterministic value noise in [-1,1] from an integer lattice (used for frizz).
float hash01(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return float(x & 0xFFFFFFu) / float(0x1000000u);
}
float valueNoise(float t, uint32_t salt)
{
    const float f = std::floor(t);
    const float u = smooth01(t - f);
    const uint32_t i = uint32_t(int(f));
    const float a = hash01(i * 2654435761u + salt) * 2.0f - 1.0f;
    const float b = hash01((i + 1) * 2654435761u + salt) * 2.0f - 1.0f;
    return a + (b - a) * u;
}

V3 vertexOf(const std::vector<float>& p, size_t i) { return {p[i * 3], p[i * 3 + 1], p[i * 3 + 2]}; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------
// Rng
// ---------------------------------------------------------------------------------------------------------------------------

uint64_t Rng::next()
{
    m_s += 0x9E3779B97F4A7C15ull;
    uint64_t z = m_s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
float Rng::uniform() { return float(next() >> 40) / float(1ull << 24); }
float Rng::gauss()
{
    float s = 0.0f;
    for (int i = 0; i < 12; ++i) s += uniform();
    return clampf(s - 6.0f, -3.0f, 3.0f);
}

// ---------------------------------------------------------------------------------------------------------------------------
// BodyCollider
// ---------------------------------------------------------------------------------------------------------------------------

void BodyCollider::build(const std::vector<float>& positions, const std::vector<std::array<uint32_t, 3>>& triangles)
{
    const size_t n = positions.size() / 3;
    m_pos.assign(n, V3());
    m_nrm.assign(n, V3());
    m_grid.clear();
    if (n == 0) return;
    V3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
    for (size_t i = 0; i < n; ++i) {
        m_pos[i] = vertexOf(positions, i);
        lo = {std::min(lo.x, m_pos[i].x), std::min(lo.y, m_pos[i].y), std::min(lo.z, m_pos[i].z)};
        hi = {std::max(hi.x, m_pos[i].x), std::max(hi.y, m_pos[i].y), std::max(hi.z, m_pos[i].z)};
    }
    for (const auto& t : triangles) {
        if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
        const V3 fn = cross(m_pos[t[1]] - m_pos[t[0]], m_pos[t[2]] - m_pos[t[0]]);   // area weighted
        for (uint32_t k : t) m_nrm[k] += fn;
    }
    for (V3& v : m_nrm) v = normalized(v);
    m_min = lo;
    const V3 ext = hi - lo;
    m_cell = std::max(0.015f, std::max(ext.x, std::max(ext.y, ext.z)) / 64.0f);
    m_nx = std::max(1, int(ext.x / m_cell) + 1);
    m_ny = std::max(1, int(ext.y / m_cell) + 1);
    m_nz = std::max(1, int(ext.z / m_cell) + 1);
    m_grid.assign(size_t(m_nx) * size_t(m_ny) * size_t(m_nz), {});
    for (size_t i = 0; i < n; ++i) {
        const int ix = std::min(m_nx - 1, int((m_pos[i].x - lo.x) / m_cell));
        const int iy = std::min(m_ny - 1, int((m_pos[i].y - lo.y) / m_cell));
        const int iz = std::min(m_nz - 1, int((m_pos[i].z - lo.z) / m_cell));
        m_grid[size_t(cellIndex(ix, iy, iz))].push_back(int(i));
    }
}

int BodyCollider::nearest(const V3& p, float* distance) const
{
    if (m_pos.empty()) return -1;
    const int cx = int(std::floor((p.x - m_min.x) / m_cell));
    const int cy = int(std::floor((p.y - m_min.y) / m_cell));
    const int cz = int(std::floor((p.z - m_min.z) / m_cell));
    int best = -1;
    float bestD2 = 1e30f;
    const int maxRing = std::max(m_nx, std::max(m_ny, m_nz));
    for (int ring = 0; ring <= maxRing; ++ring) {
        for (int dz = -ring; dz <= ring; ++dz)
            for (int dy = -ring; dy <= ring; ++dy) {
                // only the cells ON the shell of this ring: a row that is not on a z / y face of the shell touches just its two x ends (scanning
                // the whole cube and skipping the inside made the search cost grow with the 4th power of the ring count)
                const bool onFace = std::abs(dz) == ring || std::abs(dy) == ring;
                const int dxStep = onFace || ring == 0 ? 1 : 2 * ring;
                for (int dx = -ring; dx <= ring; dx += dxStep) {
                    const int ix = cx + dx, iy = cy + dy, iz = cz + dz;
                    if (ix < 0 || iy < 0 || iz < 0 || ix >= m_nx || iy >= m_ny || iz >= m_nz) continue;
                    for (int v : m_grid[size_t(cellIndex(ix, iy, iz))]) {
                        const V3 d = m_pos[size_t(v)] - p;
                        const float d2 = dot(d, d);
                        if (d2 < bestD2) { bestD2 = d2; best = v; }
                    }
                }
            }
        // a ring further out cannot hold anything closer than (ring * cell)
        if (best >= 0 && std::sqrt(bestD2) <= float(ring) * m_cell) break;
    }
    if (distance) *distance = std::sqrt(bestD2);
    return best;
}

float BodyCollider::signedDistance(const V3& p) const
{
    float d = 0.0f;
    const int v = nearest(p, &d);
    if (v < 0) return 1e9f;
    return dot(p - m_pos[size_t(v)], m_nrm[size_t(v)]);
}

V3 BodyCollider::pushOut(const V3& p, float margin, bool avoidEars) const
{
    V3 q = p;
    float d = 0.0f;
    const int v = nearest(q, &d);
    if (v < 0) return p;
    const float s = dot(q - m_pos[size_t(v)], m_nrm[size_t(v)]);
    if (s < margin) q = q + m_nrm[size_t(v)] * (margin - s);
    if (avoidEars)
        for (const Sphere& sp : m_spheres) {
            const V3 rel = q - sp.c;
            const float dist = length(rel);
            if (dist >= sp.r + margin) continue;
            V3 dir = dist > 1e-6f ? rel * (1.0f / dist) : V3(sp.side, 0, 0);
            dir.y *= 0.15f;                                          // sideways, not up: pushed up the strand's new direction pointed up and curled (a spike by the ear)
            if (dir.x * sp.side < 0.5f) dir.x = sp.side * 0.5f;      // over the ear (away from the head), never in towards the skull
            dir = normalized(dir, V3(sp.side, 0, 0));
            q = sp.c + dir * (sp.r + margin);
        }
    return q;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Scalp
// ---------------------------------------------------------------------------------------------------------------------------

V3 Scalp::pointAt(int t, float u, float v) const
{
    const auto& f = tri[size_t(t)];
    return pos[size_t(f[0])] * (1 - u - v) + pos[size_t(f[1])] * u + pos[size_t(f[2])] * v;
}
V3 Scalp::normalAt(int t, float u, float v) const
{
    const auto& f = tri[size_t(t)];
    return normalized(nrm[size_t(f[0])] * (1 - u - v) + nrm[size_t(f[1])] * u + nrm[size_t(f[2])] * v);
}

float hairlineFraction(float xn, float zn, float offset)
{
    const float front = smoothstep(0.2f, 0.9f, zn);
    const float back = smoothstep(0.2f, 0.9f, -zn);
    const float side = smoothstep(0.55f, 1.0f, std::fabs(xn)) * (1.0f - front);
    float f = 0.62f + (0.80f - 0.62f) * front + (0.36f - 0.62f) * back;
    f += 0.04f * side;                    // just above the ears
    return clampf(f + offset, 0.2f, 0.95f);
}

HeadFrame estimateHeadFrame(const std::vector<float>& positions, const std::vector<uint8_t>& mask)
{
    HeadFrame h;
    const size_t n = positions.size() / 3;
    float top = -1e9f;
    size_t count = 0;
    for (size_t i = 0; i < n && i < mask.size(); ++i)
        if (mask[i]) { top = std::max(top, positions[i * 3 + 1]); ++count; }
    if (count < 30) return h;
    // width / depth of the skull from the vertices in the top 0.10 m (above the ears and the jaw)
    float xmin = 1e9f, xmax = -1e9f, zmin = 1e9f, zmax = -1e9f;
    for (size_t i = 0; i < n && i < mask.size(); ++i) {
        if (!mask[i] || positions[i * 3 + 1] < top - 0.10f) continue;
        xmin = std::min(xmin, positions[i * 3]); xmax = std::max(xmax, positions[i * 3]);
        zmin = std::min(zmin, positions[i * 3 + 2]); zmax = std::max(zmax, positions[i * 3 + 2]);
    }
    if (xmax <= xmin || zmax <= zmin) return h;
    const float width = xmax - xmin;
    const float height = clampf(1.5f * width, 0.17f, 0.30f);
    h.top = top;
    h.chin = top - height;
    h.centre = {(xmin + xmax) * 0.5f, top - height * 0.5f, (zmin + zmax) * 0.5f};
    h.radii = {width * 0.5f, height * 0.5f, (zmax - zmin) * 0.5f};
    h.valid = true;
    return h;
}

Scalp extractScalp(const std::vector<float>& positions, const std::vector<std::array<uint32_t, 3>>& triangles, const std::vector<uint8_t>& mask, float hairlineOffset)
{
    Scalp s;
    s.head = estimateHeadFrame(positions, mask);
    if (!s.head.valid) return s;
    const size_t n = positions.size() / 3;
    const float H = s.head.top - s.head.chin;
    std::vector<int> remap(n, -1);
    std::vector<V3> normals(n);
    for (const auto& t : triangles) {
        if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
        const V3 fn = cross(vertexOf(positions, t[1]) - vertexOf(positions, t[0]), vertexOf(positions, t[2]) - vertexOf(positions, t[0]));
        for (uint32_t k : t) normals[k] += fn;
    }
    auto above = [&](size_t v) {
        if (v >= mask.size() || !mask[v]) return false;
        const V3 p = vertexOf(positions, v);
        const float xn = (p.x - s.head.centre.x) / s.head.radii.x;
        const float zn = (p.z - s.head.centre.z) / s.head.radii.z;
        return (p.y - s.head.chin) / H >= hairlineFraction(xn, zn, hairlineOffset);
    };
    auto use = [&](uint32_t v) {
        if (remap[v] < 0) {
            remap[v] = int(s.pos.size());
            s.pos.push_back(vertexOf(positions, v));
            s.nrm.push_back(normalized(normals[v]));
        }
        return remap[v];
    };
    for (const auto& t : triangles) {
        if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
        if (!above(t[0]) || !above(t[1]) || !above(t[2])) continue;
        s.tri.push_back({use(t[0]), use(t[1]), use(t[2])});
        const V3 a = s.pos[size_t(s.tri.back()[0])], b = s.pos[size_t(s.tri.back()[1])], c = s.pos[size_t(s.tri.back()[2])];
        s.triArea.push_back(0.5f * length(cross(b - a, c - a)));
        s.totalArea += s.triArea.back();
    }
    return s;
}

void addEarColliders(BodyCollider* collider, const std::vector<float>& positions, const std::vector<uint8_t>& mask)
{
    const HeadFrame h = estimateHeadFrame(positions, mask);
    if (!collider || !h.valid) return;
    const float H = h.top - h.chin;
    for (const float side : {-1.0f, 1.0f}) {
        V3 sum;
        int n = 0;
        std::vector<V3> pts;
        for (size_t i = 0; i * 3 + 2 < positions.size() && i < mask.size(); ++i) {
            if (!mask[i]) continue;
            const V3 v = vertexOf(positions, uint32_t(i));
            const float dx = (v.x - h.centre.x) * side;
            if (dx < 1.04f * h.radii.x || v.y < h.chin + 0.2f * H || v.y > h.top - 0.05f) continue;   // beyond the skull's side, at ear height
            pts.push_back(v);
            sum += v;
            ++n;
        }
        if (n < 8) continue;
        const V3 c = sum * (1.0f / float(n));
        float r = 0.0f;
        for (const V3& v : pts) r = std::max(r, length(v - c));
        collider->addSphere(c, clampf(r * 0.7f + 0.003f, 0.015f, 0.035f), side);   // hair lying over an ear needs ~2 cm, not the whole ear's extent: a larger ball made flat panels stand off beside the face
    }
}

std::vector<float> scalpWeights(const std::vector<float>& positions, const std::vector<uint8_t>& mask, float hairlineOffset)
{
    std::vector<float> w(positions.size() / 3, 0.0f);
    const HeadFrame h = estimateHeadFrame(positions, mask);
    if (!h.valid) return w;
    const float H = h.top - h.chin;
    for (size_t i = 0; i < w.size() && i < mask.size(); ++i) {
        if (!mask[i]) continue;
        const V3 v = vertexOf(positions, uint32_t(i));
        const float xn = (v.x - h.centre.x) / h.radii.x, zn = (v.z - h.centre.z) / h.radii.z;
        const float hf = (v.y - h.chin) / H;
        const float hl = hairlineFraction(xn, zn, hairlineOffset);
        w[i] = smoothstep(hl - 0.03f, hl + 0.05f, hf) * (1.0f - smoothstep(1.0f, 1.08f, std::fabs(xn)));   // soft hairline; not on the ears
    }
    return w;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Parameters, presets, JSON
// ---------------------------------------------------------------------------------------------------------------------------

namespace {

struct PresetDef {
    const char* id;
    const char* label;
};
const PresetDef kPresets[] = {
    {"short_straight", "Short straight"}, {"medium_layered", "Medium layered"}, {"long_straight", "Long straight"},
    {"wavy", "Wavy"},                     {"curly", "Curly"},                   {"bob", "Bob"},
    {"ponytail", "Ponytail"},             {"short_side_part", "Short side part"},
    {"pixie", "Pixie"},                   {"buzz", "Buzz cut"},                 {"afro", "Afro"},
    {"curly_bob", "Curly bob"},           {"shag", "Shag"},                     {"long_wavy", "Long wavy"},
    {"high_ponytail", "High ponytail"},   {"low_ponytail", "Low ponytail"},     {"side_swept_long", "Side-swept long"},
};

}  // namespace

float effectiveHairlineOffset(const Params& p) { return p.hairlineOffset + 0.04f * clampf(p.ponytail, 0.0f, 1.0f); }

QStringList presetIds()
{
    QStringList l;
    for (const PresetDef& d : kPresets) l << QString::fromLatin1(d.id);
    return l;
}
QString presetLabel(const QString& id)
{
    for (const PresetDef& d : kPresets)
        if (id == QLatin1String(d.id)) return QString::fromLatin1(d.label);
    return id;
}
bool isPreset(const QString& id) { return presetIds().contains(id); }

Params presetParams(const QString& id)
{
    Params p;
    p.preset = id;
    if (id == QLatin1String("short_straight")) {
        p.length = 0.075f; p.density = 0.7f; p.volume = 0.35f; p.layers = 0.25f; p.taper = 0.35f; p.fringe = 0.12f; p.gravity = 0.5f; p.parting = 0.15f;
    } else if (id == QLatin1String("medium_layered")) {
        p.length = 0.20f; p.density = 0.7f; p.volume = 0.45f; p.layers = 0.55f; p.fringe = 0.12f; p.gravity = 0.7f; p.frizz = 0.15f; p.clumping = 0.55f;
    } else if (id == QLatin1String("long_straight")) {
        p.length = 0.42f; p.density = 0.75f; p.volume = 0.30f; p.layers = 0.15f; p.gravity = 0.85f; p.parting = 0.0f; p.frizz = 0.05f; p.clumping = 0.45f;
    } else if (id == QLatin1String("wavy")) {
        p.length = 0.32f; p.density = 0.75f; p.volume = 0.55f; p.curliness = 0.35f; p.layers = 0.3f; p.gravity = 0.55f; p.frizz = 0.2f; p.clumping = 0.6f;
    } else if (id == QLatin1String("curly")) {
        p.length = 0.22f; p.density = 0.85f; p.volume = 0.85f; p.curliness = 0.85f; p.layers = 0.25f; p.gravity = 0.3f; p.frizz = 0.35f; p.clumping = 0.7f; p.thickness = 0.042f;
    } else if (id == QLatin1String("bob")) {
        p.length = 0.15f; p.density = 0.8f; p.volume = 0.5f; p.gravity = 0.8f; p.tipCurl = 0.6f; p.fringe = 0.3f; p.frizz = 0.04f; p.taper = 0.15f; p.parting = 0.1f;
    } else if (id == QLatin1String("ponytail")) {
        p.length = 0.34f; p.density = 0.7f; p.volume = 0.3f; p.ponytail = 1.0f; p.tieHeight = 0.5f; p.gravity = 0.7f; p.fringe = 0.15f; p.clumping = 0.7f; p.frizz = 0.06f; p.symmetric = true;
    } else if (id == QLatin1String("short_side_part")) {
        p.length = 0.055f; p.density = 0.7f; p.volume = 0.4f; p.taper = 0.8f; p.parting = 0.45f; p.fringe = 0.1f; p.gravity = 0.45f; p.frizz = 0.06f; p.layers = 0.1f;
    } else if (id == QLatin1String("pixie")) {
        p.length = 0.065f; p.density = 0.75f; p.volume = 0.5f; p.layers = 0.5f; p.taper = 0.7f; p.fringe = 0.55f; p.parting = 0.35f; p.gravity = 0.35f; p.frizz = 0.12f; p.clumping = 0.6f;
    } else if (id == QLatin1String("buzz")) {
        p.length = 0.022f; p.density = 0.6f; p.volume = 0.1f; p.taper = 0.5f; p.gravity = 0.2f; p.frizz = 0.03f; p.clumping = 0.1f; p.fringe = 0.0f;
    } else if (id == QLatin1String("afro")) {
        p.length = 0.13f; p.density = 0.9f; p.volume = 0.95f; p.curliness = 0.95f; p.gravity = 0.05f; p.frizz = 0.4f; p.clumping = 0.8f; p.layers = 0.0f; p.thickness = 0.05f;
    } else if (id == QLatin1String("curly_bob")) {
        p.length = 0.17f; p.density = 0.85f; p.volume = 0.8f; p.curliness = 0.75f; p.gravity = 0.35f; p.frizz = 0.3f; p.clumping = 0.7f; p.tipCurl = 0.3f; p.thickness = 0.042f;
    } else if (id == QLatin1String("shag")) {
        p.length = 0.22f; p.density = 0.8f; p.volume = 0.6f; p.layers = 0.85f; p.fringe = 0.5f; p.gravity = 0.55f; p.frizz = 0.3f; p.clumping = 0.75f; p.curliness = 0.15f; p.taper = 0.1f;
    } else if (id == QLatin1String("long_wavy")) {
        p.length = 0.48f; p.density = 0.8f; p.volume = 0.5f; p.curliness = 0.4f; p.layers = 0.2f; p.gravity = 0.7f; p.frizz = 0.15f; p.clumping = 0.6f; p.parting = 0.2f;
    } else if (id == QLatin1String("high_ponytail")) {
        p.length = 0.36f; p.density = 0.7f; p.volume = 0.3f; p.ponytail = 1.0f; p.tieHeight = 0.88f; p.gravity = 0.6f; p.fringe = 0.25f; p.clumping = 0.7f; p.frizz = 0.06f;
    } else if (id == QLatin1String("low_ponytail")) {
        p.length = 0.40f; p.density = 0.7f; p.volume = 0.25f; p.ponytail = 1.0f; p.tieHeight = 0.28f; p.gravity = 0.8f; p.parting = 0.3f; p.clumping = 0.7f; p.frizz = 0.05f;
    } else if (id == QLatin1String("side_swept_long")) {
        p.length = 0.38f; p.density = 0.75f; p.volume = 0.4f; p.parting = 0.7f; p.fringe = 0.45f; p.layers = 0.3f; p.gravity = 0.75f; p.frizz = 0.08f; p.clumping = 0.5f;
    }
    return p;
}

void sanitize(Params& p)
{
    auto fin = [](float v, float def) { return std::isfinite(v) ? v : def; };
    p.length = clampf(fin(p.length, 0.1f), 0.01f, 1.2f);
    p.density = clampf(fin(p.density, 0.6f), 0.0f, 1.0f);
    p.volume = clampf(fin(p.volume, 0.4f), 0.0f, 1.0f);
    p.curliness = clampf(fin(p.curliness, 0.0f), 0.0f, 1.0f);
    p.clumping = clampf(fin(p.clumping, 0.5f), 0.0f, 1.0f);
    p.frizz = clampf(fin(p.frizz, 0.1f), 0.0f, 1.0f);
    p.parting = clampf(fin(p.parting, 0.0f), -1.0f, 1.0f);
    p.layers = clampf(fin(p.layers, 0.0f), 0.0f, 1.0f);
    p.taper = clampf(fin(p.taper, 0.0f), 0.0f, 1.0f);
    p.thickness = clampf(fin(p.thickness, 0.034f), 0.004f, 0.12f);
    p.fringe = clampf(fin(p.fringe, 0.0f), 0.0f, 1.0f);
    p.tipCurl = clampf(fin(p.tipCurl, 0.0f), -1.0f, 1.0f);
    p.gravity = clampf(fin(p.gravity, 0.6f), 0.0f, 1.0f);
    p.hairlineOffset = clampf(fin(p.hairlineOffset, 0.0f), -0.15f, 0.15f);
    p.whorl = clampf(fin(p.whorl, 0.7f), 0.0f, 1.0f);
    p.ponytail = clampf(fin(p.ponytail, 0.0f), 0.0f, 1.0f);
    p.tieHeight = clampf(fin(p.tieHeight, 0.55f), 0.2f, 0.95f);
    p.quality = std::max(0, std::min(2, p.quality));
    p.maxTriangles = std::max(200, std::min(200000, p.maxTriangles));
}

std::vector<ParamSpec> paramSpecs()
{
    return {{"length", "Length (m)", 0.03f, 0.8f, 0.01f},     {"density", "Density", 0.0f, 1.0f, 0.01f},     {"volume", "Volume", 0.0f, 1.0f, 0.01f},
            {"curliness", "Curliness", 0.0f, 1.0f, 0.01f},    {"clumping", "Clumping", 0.0f, 1.0f, 0.01f},   {"frizz", "Frizz", 0.0f, 1.0f, 0.01f},
            {"parting", "Parting", -1.0f, 1.0f, 0.01f},       {"layers", "Layers", 0.0f, 1.0f, 0.01f},       {"taper", "Taper (fade)", 0.0f, 1.0f, 0.01f},
            {"thickness", "Card width (m)", 0.008f, 0.1f, 0.001f}, {"fringe", "Fringe", 0.0f, 1.0f, 0.01f}, {"tipCurl", "Tip curl", -1.0f, 1.0f, 0.01f},
            {"gravity", "Gravity", 0.0f, 1.0f, 0.01f},        {"hairlineOffset", "Hairline", -0.15f, 0.15f, 0.005f}, {"whorl", "Crown whorl", 0.0f, 1.0f, 0.01f}, {"ponytail", "Ponytail", 0.0f, 1.0f, 0.01f},
            {"tieHeight", "Tie height", 0.2f, 0.95f, 0.01f},  {"quality", "Quality (0-2)", 0.0f, 2.0f, 1.0f}, {"seed", "Random seed", 0.0f, 9999.0f, 1.0f}};
}

bool setParam(Params& p, const QString& key, double v)
{
    if (!std::isfinite(v)) return false;
    const float f = float(v);
    float* const fields[] = {&p.length, &p.density, &p.volume, &p.curliness, &p.clumping, &p.frizz, &p.parting, &p.layers, &p.taper, &p.thickness,
                             &p.fringe, &p.tipCurl, &p.gravity, &p.hairlineOffset, &p.ponytail, &p.tieHeight, &p.whorl};
    static const char* const names[] = {"length", "density", "volume", "curliness", "clumping", "frizz", "parting", "layers", "taper", "thickness",
                                        "fringe", "tipCurl", "gravity", "hairlineOffset", "ponytail", "tieHeight", "whorl"};
    for (size_t i = 0; i < std::size(names); ++i)
        if (key == QLatin1String(names[i])) { *fields[i] = f; sanitize(p); return true; }
    // clamped BEFORE the integer conversion: a finite 1e100 would be undefined behaviour
    if (key == QLatin1String("quality")) { p.quality = int(std::lround(std::clamp(v, 0.0, 2.0))); sanitize(p); return true; }
    if (key == QLatin1String("seed")) { p.seed = uint32_t(std::clamp(v, 0.0, 4294967295.0)); return true; }
    if (key == QLatin1String("maxTriangles")) { p.maxTriangles = int(std::clamp(v, 200.0, 200000.0)); sanitize(p); return true; }
    if (key == QLatin1String("symmetric")) { p.symmetric = v != 0.0; return true; }
    return false;
}

double getParam(const Params& p, const QString& key)
{
    const QJsonValue v = toJson(p).value(key);
    return v.isBool() ? (v.toBool() ? 1.0 : 0.0) : v.toDouble(0.0);
}

QJsonObject toJson(const Params& p)
{
    QJsonObject o;
    o["schema"] = QStringLiteral("qtmesh-hair-procedural-v1");
    o["preset"] = p.preset;
    o["length"] = double(p.length); o["density"] = double(p.density); o["volume"] = double(p.volume);
    o["curliness"] = double(p.curliness); o["clumping"] = double(p.clumping); o["frizz"] = double(p.frizz);
    o["parting"] = double(p.parting); o["layers"] = double(p.layers); o["taper"] = double(p.taper);
    o["thickness"] = double(p.thickness); o["fringe"] = double(p.fringe); o["tipCurl"] = double(p.tipCurl);
    o["gravity"] = double(p.gravity); o["hairlineOffset"] = double(p.hairlineOffset);
    o["ponytail"] = double(p.ponytail); o["tieHeight"] = double(p.tieHeight); o["whorl"] = double(p.whorl);
    o["quality"] = p.quality; o["maxTriangles"] = p.maxTriangles; o["symmetric"] = p.symmetric;
    o["seed"] = double(p.seed);
    QJsonArray edits;
    for (const GuideEdit& e : p.edits)
        edits.append(QJsonObject{{"guide", e.guide}, {"cp", e.controlPoint}, {"dx", double(e.delta.x)}, {"dy", double(e.delta.y)}, {"dz", double(e.delta.z)}});
    o["edits"] = edits;
    return o;
}

bool fromJson(const QJsonObject& o, Params* out, QString* error)
{
    if (!out || o.isEmpty()) {
        if (error) *error = QStringLiteral("empty hair parameters");
        return false;
    }
    Params p = presetParams(o.value("preset").toString());
    if (!o.value("preset").isString()) p = Params();
    auto num = [&](const char* key, float& dst) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isDouble()) dst = float(v.toDouble());
    };
    num("length", p.length); num("density", p.density); num("volume", p.volume); num("curliness", p.curliness);
    num("clumping", p.clumping); num("frizz", p.frizz); num("parting", p.parting); num("layers", p.layers);
    num("taper", p.taper); num("thickness", p.thickness); num("fringe", p.fringe); num("tipCurl", p.tipCurl);
    num("gravity", p.gravity); num("hairlineOffset", p.hairlineOffset); num("ponytail", p.ponytail); num("tieHeight", p.tieHeight); num("whorl", p.whorl);
    if (o.value("quality").isDouble()) p.quality = int(std::lround(std::clamp(o.value("quality").toDouble(), 0.0, 2.0)));
    if (o.value("maxTriangles").isDouble()) p.maxTriangles = int(std::clamp(o.value("maxTriangles").toDouble(), 200.0, 200000.0));
    if (o.value("symmetric").isBool()) p.symmetric = o.value("symmetric").toBool();
    if (o.value("seed").isDouble()) p.seed = uint32_t(std::clamp(o.value("seed").toDouble(), 0.0, 4294967295.0));
    p.edits.clear();
    for (const QJsonValue& v : o.value("edits").toArray()) {
        const QJsonObject e = v.toObject();
        GuideEdit g;
        g.guide = e.value("guide").toInt();
        g.controlPoint = e.value("cp").toInt(-1);
        g.delta = {float(e.value("dx").toDouble()), float(e.value("dy").toDouble()), float(e.value("dz").toDouble())};
        if (g.guide >= 0 && finite(g.delta)) p.edits.push_back(g);
    }
    p.preset = o.value("preset").toString(p.preset);
    sanitize(p);
    *out = p;
    return true;
}

QString toJsonString(const Params& p) { return QString::fromUtf8(QJsonDocument(toJson(p)).toJson(QJsonDocument::Compact)); }

bool fromJsonString(const QString& s, Params* out, QString* error)
{
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(s.toUtf8(), &pe);
    if (!d.isObject()) {
        if (error) *error = pe.error != QJsonParseError::NoError ? pe.errorString() : QStringLiteral("not a JSON object");
        return false;
    }
    return fromJson(d.object(), out, error);
}

// ---------------------------------------------------------------------------------------------------------------------------
// Curves
// ---------------------------------------------------------------------------------------------------------------------------

V3 evalCurve(const std::vector<V3>& cp, float t)
{
    const int n = int(cp.size());
    if (n == 0) return V3();
    if (n == 1) return cp[0];
    t = clampf(t, 0.0f, 1.0f);
    const float f = t * float(n - 1);
    const int i = std::min(n - 2, int(f));
    const float u = f - float(i);
    const V3 p0 = cp[size_t(std::max(0, i - 1))], p1 = cp[size_t(i)], p2 = cp[size_t(i + 1)], p3 = cp[size_t(std::min(n - 1, i + 2))];
    const float u2 = u * u, u3 = u2 * u;
    return (p1 * 2.0f + (p2 - p0) * u + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * u2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * u3) * 0.5f;
}

float curveLength(const std::vector<V3>& cp)
{
    float l = 0.0f;
    for (size_t i = 1; i < cp.size(); ++i) l += length(cp[i] - cp[i - 1]);
    return l;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Guides
// ---------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr int kControlPoints = 9;

struct Roots {
    std::vector<V3> pos, nrm;
};

// A scalp point, area-weighted and deterministic.
bool sampleScalp(const Scalp& s, Rng& rng, V3* p, V3* n)
{
    if (!s.valid() || s.totalArea <= 0) return false;
    float pick = rng.uniform() * s.totalArea;
    size_t t = 0;
    for (; t + 1 < s.tri.size(); ++t) {
        if (pick < s.triArea[t]) break;
        pick -= s.triArea[t];
    }
    float u = rng.uniform(), v = rng.uniform();
    if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
    *p = s.pointAt(int(t), u, v);
    *n = s.normalAt(int(t), u, v);
    return true;
}

// 0 chin .. 1 crown
float heightFraction(const HeadFrame& h, const V3& p) { return clampf((p.y - h.chin) / std::max(1e-4f, h.top - h.chin), 0.0f, 1.0f); }

// Length multiplier of a root: layers shorten the crown; the taper shortens sides and back.
float lengthFactor(const Params& p, const HeadFrame& h, const V3& root)
{
    const float hn = smoothstep(0.55f, 1.0f, heightFraction(h, root));     // 0 low, 1 crown
    const float zn = (root.z - h.centre.z) / h.radii.z;
    const float front = smoothstep(0.1f, 0.9f, zn);
    float f = 1.0f - p.layers * 0.55f * hn;
    const float lowSide = 1.0f - smoothstep(0.62f, 0.95f, heightFraction(h, root));   // 1 near the hairline, 0 on the top
    f *= 1.0f - p.taper * 0.7f * lowSide * (1.0f - 0.6f * front);
    // a fringe stops above the eyes: front hair may hang only a little past its own hairline, however far back it grows
    if (p.fringe > 0.05f && zn > 0.15f) {
        const float yHairline = h.chin + hairlineFraction(root.x == h.centre.x ? 0.0f : (root.x - h.centre.x) / h.radii.x, zn, p.hairlineOffset) * (h.top - h.chin);
        const float cap = 0.035f + 1.4f * std::max(0.0f, root.y - yHairline);
        const float w = smoothstep(0.15f, 0.5f, zn) * clampf(p.fringe * 3.0f, 0.0f, 1.0f);
        f = std::min(f, lerpf(f, cap / std::max(0.03f, p.length), w));
    }
    return std::max(0.12f, f);
}

// Where the flow on the head surface points (a unit vector tangent to the surface): down the sides, radially out from the crown,
// away from the parting line, and forward-and-sideways over the forehead for a fringe.
V3 tieTarget(const Params& p, const HeadFrame& h);

// The whorl as a unit direction on the head ellipsoid: a little to one side (which one depends on the seed) and towards the back of the crown.
V3 whorlDirection(const Params& p, const HeadFrame&)
{
    const float side = (p.seed & 1u) ? 1.0f : -1.0f;
    const float x = p.symmetric ? 0.0f : 0.22f * side, z = -0.34f;   // symmetric hair keeps the whorl on the centre line
    return V3(x, std::sqrt(std::max(0.05f, 1.0f - x * x - z * z)), z);
}

V3 flowAt(const Params& p, const HeadFrame& h, const V3& root, const V3& n)
{
    const V3 down(0, -1, 0);
    // The crown grows from a WHORL: an off-centre point at the back of the head (a side that depends on the seed) that the hair swirls
    // around, not from the exact top of the skull. Near it the flow turns tangentially; further out it runs away from it (forward over
    // the front of the head, back and down behind the whorl).
    const float whorlSide = (p.seed & 1u) ? 1.0f : -1.0f;
    const V3 wd3 = whorlDirection(p, h);
    const float wx = h.centre.x + wd3.x * h.radii.x, wz = h.centre.z + wd3.z * h.radii.z;
    V3 away(root.x - wx, 0, root.z - wz);
    const float wd = length(away) / h.radii.x;                                   // distance from the whorl in head widths
    away = normalized(away, V3(0, 0, -1));
    const V3 spin(-away.z * whorlSide, 0, away.x * whorlSide);
    const float swirl = (p.symmetric ? 0.0f : p.whorl) * 1.5f * (1.0f - smoothstep(0.1f, 0.9f, wd));      // radians, strongest at the whorl
    V3 radial = normalized(away * std::cos(swirl) + spin * std::sin(swirl), away);
    radial = normalized(lerp(away, radial, smoothstep(0.0f, 0.08f, wd)), away);
    const float top = clampf(n.y, 0.0f, 1.0f);
    V3 flow = down * (1.0f - 0.8f * top) + radial * top;
    // parting: roots on either side of the part line turn away from it
    const float partX = h.centre.x + p.parting * h.radii.x * 0.55f;
    const float side = root.x >= partX ? 1.0f : -1.0f;
    const float zn = (root.z - h.centre.z) / h.radii.z;
    const float frontness = smoothstep(0.0f, 0.9f, zn) * smoothstep(0.55f, 0.95f, heightFraction(h, root));
    flow += V3(side * (0.35f + 0.5f * std::fabs(p.parting)) * top * (0.5f + frontness), 0, 0);
    // hair that grows at the front sweeps to the side and back (over the temples) instead of falling over the face; a fringe overrides
    const float frontLow = smoothstep(0.05f, 0.65f, zn);
    if (frontLow > 0.0f) {
        const V3 sweep = normalized(V3(side * 0.9f, -0.3f, -0.4f));
        flow = lerp(flow, sweep, clampf(frontLow * (1.0f - p.fringe) * 0.85f, 0.0f, 1.0f));
    }
    if (p.fringe > 0.0f && frontness > 0.0f) {
        const V3 fringe = normalized(V3(side * 0.55f, -0.45f, 0.75f));
        flow = lerp(flow, fringe, clampf(p.fringe * frontness * 1.3f, 0.0f, 1.0f));
    }
    // hair that is tied back is combed towards the tie from its very first segment (it used to leave the forehead pointing down and loop back
    // up, a flap of hair over the forehead)
    if (p.ponytail > 0.0f) flow = lerp(flow, normalized(tieTarget(p, h) - root, flow), clampf(0.85f * p.ponytail, 0.0f, 1.0f));
    flow = flow - n * dot(flow, n);
    return normalized(flow, down);
}

V3 tieTarget(const Params& p, const HeadFrame& h)
{
    return {h.centre.x, h.chin + (h.top - h.chin) * p.tieHeight, h.centre.z - h.radii.z - 0.055f};   // clear of the skull: the tail is a tube about 5 cm across
}

Guide growGuide(const Params& p, const Scalp& scalp, const BodyCollider& col, const V3& root, const V3& n, float shellBias, float lenMul)
{
    Guide g;
    g.root = root;
    g.normal = n;
    const HeadFrame& h = scalp.head;
    const float len = p.length * lengthFactor(p, h, root) * lenMul;
    const float step = len / float(kControlPoints - 1);
    const float margin = 0.003f + 0.028f * p.volume;
    const V3 flow = flowAt(p, h, root, n);
    const float lift = 0.05f + 0.4f * p.volume;
    V3 dir = normalized(n * lift + flow * (1.0f - lift));
    V3 pos = root;
    g.cp.push_back(pos);
    const V3 tie = tieTarget(p, h);
    const float tieDist = length(tie - root);
    // how far the shell stands off the head at parameter t, in head-ellipsoid units (an afro stands far out of the head)
    const auto puffAt = [&](float t) { return (0.04f + 0.30f * p.volume + 0.55f * p.volume * p.volume * p.volume) * (0.35f + 0.65f * t); };
    float travelled = 0.0f;
    bool tail = false;
    // "front hair" is decided by where a point IS (its azimuth around the head), not by where its strand started: a strand that starts at the
    // side or the crown can travel round to the front and must stop at the brows there too
    auto frontAt = [&](const V3& q) {
        // fades out smoothly as the hair is tied back (a switch at exactly 0 made 0 and 0.01 look completely different)
        return (1.0f - clampf((p.ponytail - 0.4f) / 0.5f, 0.0f, 1.0f)) * smoothstep(0.3f, 0.7f, std::cos(std::atan2((q.x - h.centre.x) / h.radii.x, (q.z - h.centre.z) / h.radii.z)));
    };
    const float tieStrength = clampf(p.ponytail * 1.5f, 0.0f, 1.0f);   // how much the hair is tied: scales everything that changes with it
    const float rootE = [&] {
        const V3 r(root.x - h.centre.x, root.y - h.centre.y, root.z - h.centre.z);
        return std::sqrt((r.x / h.radii.x) * (r.x / h.radii.x) + (r.y / h.radii.y) * (r.y / h.radii.y) + (r.z / h.radii.z) * (r.z / h.radii.z));
    }();
    for (int k = 1; k < kControlPoints; ++k) {
        const float t = float(k) / float(kControlPoints - 1);
        // hair is stiff near the scalp and hangs more and more towards the ends
        const float w = p.gravity * (0.35f + 0.65f * smoothstep(0.0f, 0.8f, t)) * 0.6f;
        // gravity leans slightly BACKWARDS for hair at the sides of the head, so long hair drapes behind the shoulders instead of spreading over the chest
        const float sideW = smoothstep(0.55f, 1.0f, std::fabs(pos.x - h.centre.x) / h.radii.x) * (1.0f - tieStrength);
        const V3 gravityDir = normalized(V3(0, -1, -0.35f * sideW));
        const float gw = w * (tail ? 1.0f : 1.0f - 0.85f * p.ponytail);
        dir = normalized(dir * (1.0f - gw) + gravityDir * gw);
        if (p.ponytail > 0.0f && !tail) {
            const V3 to = normalized(tie - pos);
            dir = normalized(lerp(dir, to, 0.65f * p.ponytail));
            if (travelled > tieDist * 0.92f || length(tie - pos) < step * 1.2f) tail = true;
        } else if (p.ponytail > 0.0f) {
            dir = normalized(lerp(dir, normalized(V3(0, -1.0f, -0.35f)), 0.5f * p.ponytail));
        }
        if (p.tipCurl != 0.0f) {
            const V3 radial = normalized(V3(pos.x - h.centre.x, 0, pos.z - h.centre.z), V3(0, 0, -1));
            dir = normalized(dir - radial * (p.tipCurl * 0.7f * smoothstep(0.65f, 1.0f, t)));
        }
        const V3 prev = pos;
        pos = pos + dir * step;
        travelled += step;
        // Hair lies ON the head: above the jaw a strand is projected back onto a shell around the skull (a little further out for volume
        // and towards the ends), so it drapes over the crown and the sides instead of sticking out of the head like palm fronds.
        // Every one of these rules has a strength that follows the Ponytail value instead of switching off the moment it is above 0 (once a
        // strand passed the tie it used to be flagged as tail for ANY ponytail > 0, which turned off the whole head-hugging: 0.01 looked
        // nothing like 0): `tailW` is how much "free tail" this point is, so a hair that is barely tied still follows the head.
        const float tailW = tail ? tieStrength : 0.0f;
        const float shellW = 1.0f - tailW;
        if (shellW > 0.0f && pos.y > h.centre.y - h.radii.y * 0.35f) {
            const V3 rel(pos.x - h.centre.x, pos.y - h.centre.y, pos.z - h.centre.z);
            const float e = std::sqrt((rel.x / h.radii.x) * (rel.x / h.radii.x) + (rel.y / h.radii.y) * (rel.y / h.radii.y) + (rel.z / h.radii.z) * (rel.z / h.radii.z));
            const float target = rootE + shellBias + puffAt(t);
            if (e > 1e-3f && e > target) pos = lerp(pos, V3(h.centre.x + rel.x * (target / e), h.centre.y + rel.y * (target / e), h.centre.z + rel.z * (target / e)), shellW);
        }
        // The ellipsoid is only an approximation of the head: at the temples and cheeks it is wider than the real face, so hair resting on it
        // floated several cm off the face in flat panels. Above the ears the hair therefore also stays within a stand-off of the REAL head
        // surface (the same offset the shell uses, in metres).
        if (shellW > 0.0f && pos.y > h.centre.y - h.radii.y * 0.35f) {
            float dist = 0.0f;
            const int vi = col.nearest(pos, &dist);
            const float standOff = 0.006f + 0.09f * (shellBias + puffAt(t));
            if (vi >= 0 && dist > standOff) pos = pos + (col.positions()[size_t(vi)] - pos) * (((dist - standOff) / dist) * shellW);
        }
        pos = col.pushOut(pos, lerpf(margin, 0.01f, tailW));   // a tied tail is a free bundle
        // Below the ears the hair hangs free: it keeps the sideways momentum it had on the head and would flare out like a skirt. It clings to
        // the body instead (cheeks, neck, shoulders): never further than a few cm from the nearest body point (more for voluminous hair).
        if (tieStrength < 1.0f && pos.y <= h.centre.y - h.radii.y * 0.35f) {
            const int vi = col.nearest(pos);
            // a tied tail does not cling (its limit grows to 'none'), but only once the hair is REALLY tied: a linear fade made 5 % of ponytail
            // already more than double the limit, so hair flared out and spiked where it rests on the shoulders
            const float dmax = lerpf(0.012f + 0.04f * p.volume, 0.5f, smoothstep(0.35f, 1.0f, tieStrength));
            if (vi >= 0) {   // horizontally only: clinging must never lift the hair
                const V3 to = col.positions()[size_t(vi)] - pos;
                const float dh = std::sqrt(to.x * to.x + to.z * to.z);
                if (dh > dmax) pos = col.pushOut(V3(pos.x + to.x * ((dh - dmax) / dh), pos.y, pos.z + to.z * ((dh - dmax) / dh)), margin);
            }
        }
        // Once it hangs, hair does not climb: whatever the collisions above did, a strand below the crown rises at most a little per segment
        // (otherwise a push by the ear or the face turned into a curl pointing up). Fades out as the hair is tied back, which does go up.
        if (t > 0.25f && pos.y < h.centre.y + 0.3f * h.radii.y) {
            const float maxRise = 0.15f * step;
            if (pos.y - prev.y > maxRise) pos.y = lerpf(pos.y, prev.y + maxRise, 1.0f - tieStrength);
        }
        // front hair (a fringe, or hair swept over the forehead) never hangs below the eyebrows; done LAST, after the shell projection and
        // the body push, which would both move the point down again
        if (const float fw = frontAt(pos); fw > 0.0f) {
            // a FLOOR on the height that only reaches the brows where the point really is in front (fw -> 1): lerping the point towards the brow
            // line by fw dragged a strand hanging low along the cheek UP by a fraction of the whole gap on every step (long hair pointing up)
            pos.y = std::max(pos.y, lerpf(h.chin - 0.4f, h.chin + 0.70f * (h.top - h.chin), fw));
            pos = col.pushOut(pos, margin);
        }
        dir = normalized(pos - prev, dir);
        g.cp.push_back(pos);
    }
    return g;
}

}  // namespace

namespace {

int segmentsFor(int quality) { return quality <= 0 ? 4 : (quality == 1 ? 7 : 12); }

// The hair is a stack of RINGS of sheets around the head (hairline first, crown last); a ring is `cols` neighbouring guides, so the
// surface between them is continuous and the hem is a smooth function of the position, like the shells of the bundled hairstyles.
struct Layout {
    int rings = 4, cols = 48, segments = 7;
};

// The scalp liner is dropped when it would take more than a third of a tight triangle budget.
size_t capTriangles(const Params& p, const Scalp& scalp) { return scalp.tri.size() * 3 <= size_t(p.maxTriangles) ? scalp.tri.size() : 0; }

Layout layoutFor(const Params& p, size_t reserved)
{
    Layout l;
    l.rings = 3 + int(std::lround(p.density * 3.0f));
    // short hair needs more rings: each must reach the next one or the scalp shows between them
    l.rings = std::max(l.rings, std::min(9, int(std::ceil(0.13f / std::max(0.015f, p.length * 0.7f)))));
    l.cols = 36 + 2 * int(std::lround(p.density * 18.0f));
    l.segments = segmentsFor(p.quality);
    auto tris = [&] { return l.rings * l.cols * l.segments * 2 + l.cols + int(reserved); };
    // Reduce the layout until it fits: columns, then rings, and only below the smallest sensible layout the segments per column (a budget of
    // 200 triangles is accepted by `sanitize`, and the layout must never exceed a hard budget).
    while (tris() > p.maxTriangles && l.cols > 16) l.cols -= 2;
    while (tris() > p.maxTriangles && l.rings > 2) --l.rings;
    while (tris() > p.maxTriangles && l.cols > 8) l.cols -= 2;
    while (tris() > p.maxTriangles && l.segments > 2) --l.segments;
    return l;
}

}  // namespace

std::vector<Guide> generateGuides(const Params& pin, const Scalp& scalp, const BodyCollider& collider)
{
    Params p = pin;
    sanitize(p);
    std::vector<Guide> guides;
    if (!scalp.valid()) return guides;
    const HeadFrame& h = scalp.head;
    const Layout L = layoutFor(p, capTriangles(p, scalp));
    guides.reserve(size_t(L.rings * L.cols));
    for (int r = 0; r < L.rings; ++r) {
        const float frac = L.rings > 1 ? float(r) / float(L.rings - 1) : 0.0f;
        for (int c = 0; c < L.cols; ++c) {
            const float theta = 2.0f * kPi * float(c) / float(L.cols);        // 0 = the face, +x to the character's... right of the +z axis
            const float xn = std::sin(theta), zn = std::cos(theta);
            const float hl = hairlineFraction(xn, zn, effectiveHairlineOffset(p));
            // rings are spaced evenly along the meridian ARC (polar angle psi from the crown), not in height: near the crown the surface is
            // almost flat, so equal height steps would leave wide bare rings there
            const float y0 = h.chin + (hl + 0.025f) * (h.top - h.chin);
            const float psi0 = std::acos(clampf((y0 - h.centre.y) / h.radii.y, -0.999f, 0.999f));
            // Rings run from the hairline (all round the head) towards the WHORL, an off-centre point at the back of the crown, not towards the
            // top of the skull: that is where hair really starts from. Interpolated on the unit sphere of the head ellipsoid; the top ring
            // ends a hair's breadth from the whorl.
            const V3 u0(xn * std::sin(psi0), std::cos(psi0), zn * std::sin(psi0));
            const V3 uw = whorlDirection(p, h);
            const float f = 0.97f * frac;
            const V3 u = normalized(lerp(u0, uw, f), uw);
            V3 root(h.centre.x + u.x * h.radii.x, h.centre.y + u.y * h.radii.y, h.centre.z + u.z * h.radii.z);
            const V3 nrm = normalized(V3(u.x / h.radii.x, u.y / h.radii.y, u.z / h.radii.z));
            root = collider.pushOut(root, 0.002f, false);
            const float psi = std::acos(clampf(dot(u0, uw), -1.0f, 1.0f));        // angular distance hairline -> whorl (for the ring spacing)
            // a smooth, slow irregularity of the hem (never per-strand noise: that is what made the old ends look like stairs)
            const float thetaN = p.symmetric ? std::min(theta, 2.0f * kPi - theta) : theta;      // mirror partners must get the same hem
            const float hem = 1.0f + (0.06f + 0.14f * p.frizz) * (valueNoise(thetaN * 1.7f + float(r) * 3.1f + float(p.seed % 977u) * 0.37f, 91u + p.seed) - 0.5f) * 2.0f;
            // every sheet is at least as long as the distance to the next ring up, so the rings overlap and no scalp shows between them
            const float spacing = 0.5f * (h.radii.x + h.radii.z) * psi * 0.97f / float(std::max(1, L.rings - 1));
            const float base = std::max(1e-3f, p.length * lengthFactor(p, h, root));
            // ONE hem: a ring higher up has to be longer by the height between its root and the hairline root of the same column, otherwise every
            // ring ends higher than the one below it and the hair reads as stacked tiers (a palm tree). `layers` raises the hem gradually.
            const float yRef = h.centre.y + std::cos(psi0) * h.radii.y;
            const float extra = std::max(0.0f, root.y - yRef) * 1.25f * (1.0f - 0.6f * p.layers * frac);
            guides.push_back(growGuide(p, scalp, collider, root, nrm, 0.012f * frac, std::max(hem * (base + extra) / base, 2.0f * spacing / base)));
        }
    }
    for (const GuideEdit& e : p.edits) applyGuideEdit(guides, e);
    return guides;
}

void applyGuideEdit(std::vector<Guide>& guides, const GuideEdit& e)
{
    if (e.guide < 0 || size_t(e.guide) >= guides.size() || !finite(e.delta)) return;
    Guide& g = guides[size_t(e.guide)];
    if (e.controlPoint < 0) {
        const float scale = std::max(0.1f, 1.0f + e.delta.x);
        for (size_t i = 1; i < g.cp.size(); ++i) g.cp[i] = g.root + (g.cp[i] - g.root) * scale;
        return;
    }
    if (size_t(e.controlPoint) >= g.cp.size() || e.controlPoint == 0) return;   // the root stays on the scalp
    // a control point drags the rest of the strand with it, fading out towards the tip
    for (size_t i = size_t(e.controlPoint); i < g.cp.size(); ++i) {
        const float fade = i == size_t(e.controlPoint) ? 1.0f : 0.6f;
        g.cp[i] += e.delta * fade;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// Cards
// ---------------------------------------------------------------------------------------------------------------------------

namespace {

struct CardCtx {
    const Params& p;
    const Scalp& scalp;
    const std::vector<Guide>& guides;
    const BodyCollider& col;
    int segments;
};

// Offsets along one strand: curl / wave helix, frizz.
V3 strandOffset(const CardCtx& c, float t, float arc, const V3& tangent, const V3& outward, float phase, uint32_t salt)
{
    V3 b1 = normalized(cross(tangent, outward), V3(1, 0, 0));
    V3 b2 = normalized(cross(tangent, b1), V3(0, 0, 1));
    V3 off;
    const float amp = c.p.curliness * (0.004f + 0.016f * c.p.curliness) * smoothstep(0.0f, 0.3f, t);
    if (amp > 0.0f) {
        const float freq = 1.5f + 5.5f * c.p.curliness;
        const float ang = 2.0f * kPi * freq * arc / std::max(0.05f, c.p.length) * 0.35f + phase;
        const float helix = smoothstep(0.35f, 0.8f, c.p.curliness);
        off += b1 * (amp * std::cos(ang)) + b2 * (amp * std::sin(ang) * (0.2f + 0.35f * helix));   // mostly SIDEWAYS: a sheet that curls in depth folds over itself
    }
    if (c.p.frizz > 0.0f) {
        const float fa = c.p.frizz * 0.004f * smoothstep(0.1f, 1.0f, t);
        off += b1 * (fa * valueNoise(arc * 40.0f, salt)) + b2 * (fa * valueNoise(arc * 40.0f + 17.0f, salt + 3));
    }
    return off;
}

struct StripGrid {
    int lines = 0, rows = 0;
    std::vector<V3> p;                       // row-major: p[row * lines + line]
    V3& at(int row, int line) { return p[size_t(row * lines + line)]; }
    const V3& at(int row, int line) const { return p[size_t(row * lines + line)]; }
};

// The points of one guide column of a ring (curve + curl/frizz + layering + ponytail spread).
void columnPoints(const CardCtx& c, const Guide& g, int ring, int col, int cols, std::vector<V3>* out)
{
    const HeadFrame& h = c.scalp.head;
    const int S = c.segments;
    const float theta = 2.0f * kPi * float(col) / float(cols);
    const float arcTotal = std::max(0.02f, curveLength(g.cp));
    float tTie = 2.0f;                                           // where the guide passes the tie point (the tail starts there)
    if (c.p.ponytail > 0.0f) {
        const V3 tie = tieTarget(c.p, h);
        float best = 1e9f;
        for (int k = 0; k <= 32; ++k) {
            const float d = length(evalCurve(g.cp, float(k) / 32.0f) - tie);
            if (d < best) { best = d; tTie = float(k) / 32.0f; }
        }
    }
    out->assign(size_t(S) + 1, V3());
    for (int i = 0; i <= S; ++i) {
        const float t = float(i) / float(S);
        V3 pt = evalCurve(g.cp, t);
        const V3 tan = normalized(evalCurve(g.cp, std::min(1.0f, t + 0.05f)) - evalCurve(g.cp, std::max(0.0f, t - 0.05f)), V3(0, -1, 0));
        const V3 outward = normalized(V3(pt.x - h.centre.x, 0, pt.z - h.centre.z), g.normal);
        pt += strandOffset(c, t, t * arcTotal, tan, outward, float(col) * 0.45f + float(ring) * 1.7f + hash01(c.p.seed) * 6.28f, uint32_t(ring * 977 + col) + c.p.seed * 131u);
        pt += outward * (0.002f * float(ring) * smoothstep(0.05f, 0.4f, t));              // higher rings lie over the lower ones
        if (c.p.ponytail > 0.0f) {                                                       // the tail (only past the tie) is a tube, not a flat ribbon
            const V3 b1 = normalized(cross(tan, V3(1, 0, 0)), V3(0, 0, 1));
            const V3 b2 = normalized(cross(tan, b1), V3(1, 0, 0));
            pt += (b1 * std::cos(theta) + b2 * std::sin(theta)) * (c.p.ponytail * 0.022f * smoothstep(tTie + 0.03f, tTie + 0.3f, t));
        }
        if (i > 0) pt = c.col.pushOut(pt, 0.002f + 0.0018f * float(ring));      // layers keep their order even where the body pushes them out
        (*out)[size_t(i)] = pt;
    }
}

// Even out kinks left by the body pushing hair around (creases and spikes at the ends, where long hair meets the shoulders): smooth along
// each column and across neighbouring columns of the ring, then push out of the body once more.
void smoothRing(const CardCtx& c, std::vector<std::vector<V3>>& ring, int ringIndex)
{
    const int cols = int(ring.size()), S = c.segments;
    const HeadFrame& h = c.scalp.head;
    const float yBrow = h.chin + 0.70f * (h.top - h.chin);
    auto frontAt = [&](const V3& q) {
        return (1.0f - clampf((c.p.ponytail - 0.4f) / 0.5f, 0.0f, 1.0f)) * smoothstep(0.3f, 0.7f, std::cos(std::atan2((q.x - h.centre.x) / h.radii.x, (q.z - h.centre.z) / h.radii.z)));
    };
    for (int iter = 0; iter < 2; ++iter) {
        auto src = ring;
        for (int col = 0; col < cols; ++col)
            for (int i = 1; i <= S; ++i) {
                const float t = float(i) / float(S);
                const V3& prev = src[size_t(col)][size_t(i - 1)];
                const V3 side = (src[size_t((col + cols - 1) % cols)][size_t(i)] + src[size_t((col + 1) % cols)][size_t(i)]) * 0.5f;
                V3 np = src[size_t(col)][size_t(i)];
                np = lerp(np, side, 0.45f * smoothstep(0.15f, 0.5f, t));
                if (i < S) np = lerp(np, (prev + src[size_t(col)][size_t(i + 1)]) * 0.5f, 0.35f);
                if (const float fw = frontAt(np); fw > 0.0f) np.y = std::max(np.y, lerpf(h.chin - 0.4f, yBrow, fw));   // smoothing must not undo the brow limit (a floor, see growGuide)
                ring[size_t(col)][size_t(i)] = c.col.pushOut(np, 0.002f + 0.0018f * float(ringIndex));
            }
    }
}

void emitStrip(CardMesh& m, const CardCtx& c, StripGrid& g, float uLo, float uHi)
{
    const int S = c.segments, n = g.lines - 1;
    // the locks of one strip pull together towards the tips: neighbouring strips open gaps there, so the hem reads as separate locks
    if (n >= 2 && c.p.clumping > 0.0f) {
        for (int i = 0; i <= S; ++i) {
            V3 mid;
            for (int j = 0; j <= n; ++j) mid += g.at(i, j);
            mid = mid * (1.0f / float(n + 1));
            const float k = c.p.clumping * 0.3f * smoothstep(0.4f, 1.0f, float(i) / float(S)) * smoothstep(0.06f, 0.2f, c.p.length);   // short hair: no wedges of bare scalp between locks
            for (int j = 0; j <= n; ++j) g.at(i, j) = lerp(g.at(i, j), mid, k);
        }
    }
    const uint32_t base = uint32_t(m.pos.size());
    m.rootVertex.push_back(int(base));
    const HeadFrame& h = c.scalp.head;
    for (int i = 0; i <= S; ++i) {
        const float t = float(i) / float(S);
        for (int j = 0; j <= n; ++j) {
            const V3& pt = g.at(i, j);
            const V3 dRow = g.at(std::min(S, i + 1), j) - g.at(std::max(0, i - 1), j);
            const V3 dCol = g.at(i, std::min(n, j + 1)) - g.at(i, std::max(0, j - 1));
            const V3 outward = normalized(V3(pt.x - h.centre.x, (pt.y - h.centre.y) * 0.4f, pt.z - h.centre.z));
            V3 geo = normalized(cross(dRow, dCol), outward);
            if (dot(geo, outward) < 0.35f) geo = outward;       // where the sheet folds against the body the true normal would face inward (dark patches)
            m.pos.push_back(pt);
            m.nrm.push_back(normalized(geo * 0.5f + outward * 0.5f, outward));
            m.uv.push_back(uLo + (uHi - uLo) * float(j) / float(n));
            m.uv.push_back(1.0f - t);                       // root at the top row of the texture (OBJ v = 1), like the lashes
        }
    }
    for (int i = 0; i < S; ++i)
        for (int j = 0; j < n; ++j) {
            const uint32_t a = base + uint32_t(i * (n + 1) + j), b = a + 1, ad = a + uint32_t(n + 1), bd = ad + 1;
            m.idx.insert(m.idx.end(), {a, ad, b, b, ad, bd});           // counter-clockwise seen from outside the head
        }
    ++m.cards;
}

// The crown: a fan from the top of the head to the roots of the highest ring.
void emitCrown(CardMesh& m, const CardCtx& c, const Layout& L)
{
    const HeadFrame& h = c.scalp.head;
    std::vector<V3> roots;
    V3 apex;
    for (int col = 0; col < L.cols; ++col) {
        roots.push_back(c.guides[size_t((L.rings - 1) * L.cols + col)].root);
        apex += roots.back();
    }
    apex = apex * (1.0f / float(L.cols));
    apex = c.col.pushOut(apex, 0.002f);
    const uint32_t base = uint32_t(m.pos.size());
    m.rootVertex.push_back(int(base));
    auto push = [&](const V3& p, float u) {
        const V3 o = normalized(V3(p.x - h.centre.x, (p.y - h.centre.y) * 0.4f, p.z - h.centre.z));
        m.pos.push_back(p);
        m.nrm.push_back(o);
        m.uv.push_back(u);
        m.uv.push_back(0.985f);
    };
    push(apex, 0.5f / float(kCardVariants));
    for (int col = 0; col < L.cols; ++col) push(c.col.pushOut(roots[size_t(col)], 0.002f), (0.2f + 0.6f * float(col % 5) / 4.0f) / float(kCardVariants));
    for (int col = 0; col < L.cols; ++col) m.idx.insert(m.idx.end(), {base, base + 1 + uint32_t(col), base + 1 + uint32_t((col + 1) % L.cols)});
    ++m.cards;
}

// A thin liner over the scalp (the "hair cap" of game hair): it carries the root colour and guarantees that no style, however short or
// swept, ever shows bare skin between its sheets.
void emitScalpCap(CardMesh& m, const CardCtx& c)
{
    const Scalp& sc = c.scalp;
    const uint32_t base = uint32_t(m.pos.size());
    m.rootVertex.push_back(int(base));
    for (size_t i = 0; i < sc.pos.size(); ++i) {
        m.pos.push_back(sc.pos[i] + sc.nrm[i] * 0.0015f);
        m.nrm.push_back(sc.nrm[i]);
        m.uv.push_back((0.15f + 0.7f * hash01(uint32_t(i) * 2654435761u)) / float(kCardVariants));
        m.uv.push_back(0.975f);
    }
    for (const auto& t : sc.tri) {
        const V3 &a = m.pos[base + size_t(t[0])], &b = m.pos[base + size_t(t[1])], &d = m.pos[base + size_t(t[2])];
        const V3 n = sc.nrm[size_t(t[0])] + sc.nrm[size_t(t[1])] + sc.nrm[size_t(t[2])];
        const bool flip = dot(cross(b - a, d - a), n) < 0.0f;
        m.idx.insert(m.idx.end(), flip ? std::initializer_list<uint32_t>{base + uint32_t(t[0]), base + uint32_t(t[2]), base + uint32_t(t[1])}
                                       : std::initializer_list<uint32_t>{base + uint32_t(t[0]), base + uint32_t(t[1]), base + uint32_t(t[2])});
    }
    ++m.cards;
}

}  // namespace

CardMesh generateCards(const Params& pin, const Scalp& scalp, const std::vector<Guide>& guides, const BodyCollider& collider)
{
    Params p = pin;
    sanitize(p);
    CardMesh m;
    if (!scalp.valid() || guides.empty()) return m;
    const Layout L = layoutFor(p, capTriangles(p, scalp));
    if (guides.size() != size_t(L.rings * L.cols)) return m;
    const HeadFrame& h = scalp.head;
    const CardCtx c{p, scalp, guides, collider, L.segments};
    const float spacing = 2.0f * kPi * 0.5f * (h.radii.x + h.radii.z) * 0.8f / float(L.cols);
    const int G = std::max(1, std::min(4, int(std::lround(p.thickness / std::max(1e-3f, spacing)))));   // columns per lock of hair
    for (int r = 0; r < L.rings; ++r) {
        std::vector<std::vector<V3>> ring(size_t(L.cols));
        for (int col = 0; col < L.cols; ++col) columnPoints(c, guides[size_t(r * L.cols + col)], r, col, L.cols, &ring[size_t(col)]);
        smoothRing(c, ring, r);
        for (int g0 = 0; g0 < L.cols; g0 += G) {
            const int n = std::min(G, L.cols - g0);
            StripGrid grid;
            grid.lines = n + 1;
            grid.rows = c.segments + 1;
            grid.p.assign(size_t(grid.lines * grid.rows), V3());
            for (int j = 0; j <= n; ++j)
                for (int i = 0; i <= c.segments; ++i) grid.at(i, j) = ring[size_t((g0 + j) % L.cols)][size_t(i)];
            const int variant = int(hash01(uint32_t(r * 7919 + g0 * 104729) + p.seed * 31u) * float(kCardVariants)) % kCardVariants;
            emitStrip(m, c, grid, (float(variant) + 0.02f) / float(kCardVariants), (float(variant) + 0.98f) / float(kCardVariants));
        }
    }
    emitCrown(m, c, L);
    if (capTriangles(p, scalp) > 0) emitScalpCap(m, c);
    return m;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------------------------------------------------------

ValidationReport validate(const CardMesh& m)
{
    ValidationReport r;
    for (size_t i = 0; i < m.pos.size(); ++i)
        if (!finite(m.pos[i]) || i >= m.nrm.size() || !finite(m.nrm[i])) ++r.nanVertices;     // a missing normal counts as a broken vertex
    for (size_t i = 0; i + 1 < m.uv.size(); i += 2)
        if (!std::isfinite(m.uv[i]) || !std::isfinite(m.uv[i + 1]) || m.uv[i] < -1e-4f || m.uv[i] > 1.0001f || m.uv[i + 1] < -1e-4f || m.uv[i + 1] > 1.0001f) ++r.badUvs;
    for (size_t t = 0; t + 2 < m.idx.size(); t += 3) {
        if (m.idx[t] >= m.pos.size() || m.idx[t + 1] >= m.pos.size() || m.idx[t + 2] >= m.pos.size()) { ++r.badIndices; continue; }
        const V3 &a = m.pos[m.idx[t]], &b = m.pos[m.idx[t + 1]], &c = m.pos[m.idx[t + 2]];
        if (!finite(a) || !finite(b) || !finite(c)) continue;
        if (length(cross(b - a, c - a)) < 1e-10f) ++r.zeroAreaTriangles;
    }
    return r;
}

ValidationReport repair(CardMesh& m)
{
    const ValidationReport r = validate(m);
    if (r.ok()) return r;
    std::vector<uint32_t> kept;
    kept.reserve(m.idx.size());
    for (size_t t = 0; t + 2 < m.idx.size(); t += 3) {
        const uint32_t a = m.idx[t], b = m.idx[t + 1], c = m.idx[t + 2];
        if (a >= m.pos.size() || b >= m.pos.size() || c >= m.pos.size()) continue;
        if (!finite(m.pos[a]) || !finite(m.pos[b]) || !finite(m.pos[c])) continue;
        if (length(cross(m.pos[b] - m.pos[a], m.pos[c] - m.pos[a])) < 1e-10f) continue;
        kept.push_back(a); kept.push_back(b); kept.push_back(c);
    }
    m.idx = std::move(kept);
    for (size_t i = 0; i < m.pos.size(); ++i) {
        if (i >= m.nrm.size() || !finite(m.nrm[i])) { if (i >= m.nrm.size()) m.nrm.resize(m.pos.size(), V3(0, 1, 0)); m.nrm[i] = V3(0, 1, 0); }
        if (!finite(m.pos[i])) m.pos[i] = V3();
    }
    for (float& u : m.uv) u = std::isfinite(u) ? clampf(u, 0.0f, 1.0f) : 0.0f;
    return r;
}

Result generate(const Params& p, const Scalp& scalp, const BodyCollider& collider)
{
    Result r;
    if (!scalp.valid()) {
        r.warnings << QStringLiteral("Hair: no scalp could be found on this head.");
        return r;
    }
    const std::vector<Guide> guides = generateGuides(p, scalp, collider);
    r.guides = int(guides.size());
    r.mesh = generateCards(p, scalp, guides, collider);
    const ValidationReport rep = repair(r.mesh);
    if (!rep.ok())
        r.warnings << QStringLiteral("Hair: repaired %1 non-finite vertices, %2 bad indices, %3 zero-area triangles, %4 bad UVs.")
                          .arg(rep.nanVertices).arg(rep.badIndices).arg(rep.zeroAreaTriangles).arg(rep.badUvs);
    if (r.mesh.triangles() == 0) r.warnings << QStringLiteral("Hair: the parameters produced no cards.");
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------------
// DNA mesh + texture
// ---------------------------------------------------------------------------------------------------------------------------

Dna::Mesh toDnaMesh(const CardMesh& cm, const BodyCollider& col, const std::vector<std::vector<Dna::SkinInfluence>>& bodySkin)
{
    Dna::Mesh m;
    m.name = QStringLiteral("head_hair");
    const size_t n = cm.pos.size();
    m.positions.reserve(n * 3);
    m.normals.reserve(n * 3);
    m.skin.resize(n);
    m.uvs = cm.uv;
    for (size_t i = 0; i < n; ++i) {
        m.positions.insert(m.positions.end(), {cm.pos[i].x, cm.pos[i].y, cm.pos[i].z});
        m.normals.insert(m.normals.end(), {cm.nrm[i].x, cm.nrm[i].y, cm.nrm[i].z});
        m.layouts.push_back({uint32_t(i), uint32_t(i), uint32_t(i)});
        const int v = col.nearest(cm.pos[i]);
        if (v >= 0 && size_t(v) < bodySkin.size()) m.skin[i] = bodySkin[size_t(v)];
    }
    for (size_t t = 0; t + 2 < cm.idx.size(); t += 3) m.faces.push_back({cm.idx[t], cm.idx[t + 1], cm.idx[t + 2]});
    return m;
}

QByteArray bakeCardTexturePng(int size)
{
    size = std::max(128, std::min(2048, size));
    QImage img(size, size, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    const int colW = size / kCardVariants;
    // Each column is a seamless, horizontally periodic tile of ONE lock: a bundle of fine strands running root -> tip. Every strand has
    // its own length (so the hem frays into single strands instead of ending in a straight cut or a spike), its own brightness, and the
    // lock is a little darker at its two edges and at the root so neighbouring locks read as separate.
    for (int variant = 0; variant < kCardVariants; ++variant) {
        const int strands = 30 + 4 * variant;
        const float f1 = float(7 + 2 * variant), f2 = float(15 + 3 * variant);
        for (int y = 0; y < size; ++y) {
            const float t = float(y) / float(size - 1);                  // 0 root (top row) .. 1 tip
            for (int x = 0; x < colW; ++x) {
                const float u = (float(x) + 0.5f) / float(colW);
                const int id = int(u * float(strands));
                const float own = hash01(uint32_t(id * 7919 + variant * 104729 + 13));
                const float endT = 0.80f + 0.20f * own;                  // where this strand ends
                // strands also START at slightly different distances from the root: the hairline (where the first ring begins) is feathered, not a
                // hard contour; the rings above overlap these roots, so only the hairline shows it
                const float startT = 0.075f * std::pow(hash01(uint32_t(id * 4093 + variant * 331 + 29)), 1.6f);
                const bool cut = t > endT || t < startT;                 // alpha 0 there, but the colour keeps going (see below)
                const float wob = 0.012f * std::sin(t * 9.0f + own * 6.28f);
                const float s1 = 0.5f + 0.5f * std::sin(2.0f * kPi * (f1 * (u + wob)));
                const float s2 = 0.5f + 0.5f * std::sin(2.0f * kPi * (f2 * (u - wob) + 0.3f * float(variant)));
                const float fine = hash01(uint32_t(x * 131 + y * 17 + variant * 977)) - 0.5f;
                float lum = 0.5f + 0.34f * (own - 0.5f) + 0.12f * (s1 - 0.5f) + 0.07f * (s2 - 0.5f) + 0.05f * fine;
                lum *= 0.60f + 0.40f * smoothstep(0.0f, 0.22f, t);                          // darker at the root
                lum += 0.10f * std::exp(-((t - 0.42f) / 0.13f) * ((t - 0.42f) / 0.13f));     // soft sheen
                lum *= 0.80f + 0.20f * smoothstep(0.0f, 0.14f, std::min(u, 1.0f - u));      // lock edges
                lum *= 1.0f - 0.18f * smoothstep(0.85f, 1.0f, std::min(1.0f, t / endT));        // thin, darker strand ends
                const int g = int(clampf(lum, 0.0f, 1.0f) * 255.0f);
                img.setPixel(variant * colW + x, y, qRgba(g, g, g, cut ? 0 : 255));   // cut texels keep hair-coloured RGB: without a multisample buffer alpha-to-coverage does not discard them, and black RGB showed as dark holes
            }
        }
    }
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

}  // namespace Hair
}  // namespace CharacterGen
