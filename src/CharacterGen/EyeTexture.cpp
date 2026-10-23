#include "EyeTexture.h"

#include <QBuffer>

#include <algorithm>
#include <array>
#include <cmath>

namespace CharacterGen {
namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kFrontAngle = 1.2566371f;   // 72 degrees: linear region of the UV map
constexpr float kFrontRadius = 0.92f;       // texture radius reached at kFrontAngle
constexpr float kIrisAngle = 0.52f;         // radians from the pole: the limbus (about 30 degrees of the eyeball)
constexpr float kPupilAngle = 0.20f;

float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }
float smoothstep(float a, float b, float x) { const float t = clamp01((x - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
float mix(float a, float b, float t) { return a + (b - a) * t; }

uint32_t hash3(int x, int y, int z, uint32_t seed)
{
    uint32_t h = seed * 0x9E3779B9u ^ uint32_t(x) * 0x85EBCA6Bu ^ uint32_t(y) * 0xC2B2AE35u ^ uint32_t(z) * 0x27D4EB2Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
float lattice(int x, int y, int z, uint32_t seed) { return float(hash3(x, y, z, seed) & 0xFFFFFF) / float(0xFFFFFF); }

// 3D value noise, 0..1
float noise3(float x, float y, float z, uint32_t seed)
{
    const int xi = int(std::floor(x)), yi = int(std::floor(y)), zi = int(std::floor(z));
    const float fx = x - float(xi), fy = y - float(yi), fz = z - float(zi);
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy), sz = fz * fz * (3 - 2 * fz);
    float c[2][2][2];
    for (int dx = 0; dx < 2; ++dx) for (int dy = 0; dy < 2; ++dy) for (int dz = 0; dz < 2; ++dz) c[dx][dy][dz] = lattice(xi + dx, yi + dy, zi + dz, seed);
    auto l = [](float a, float b, float t) { return a + (b - a) * t; };
    return l(l(l(c[0][0][0], c[1][0][0], sx), l(c[0][1][0], c[1][1][0], sx), sy), l(l(c[0][0][1], c[1][0][1], sx), l(c[0][1][1], c[1][1][1], sx), sy), sz);
}

float fbm(float x, float y, float z, uint32_t seed, int octaves)
{
    float sum = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * noise3(x, y, z, seed + uint32_t(o) * 101u);
        norm += amp;
        x *= 2.03f; y *= 2.03f; z *= 2.03f; amp *= 0.5f;
    }
    return sum / norm;
}

using V3 = std::array<float, 3>;
V3 lerp3(const V3& a, const V3& b, float t) { return {mix(a[0], b[0], t), mix(a[1], b[1], t), mix(a[2], b[2], t)}; }
V3 scale3(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
}  // namespace

float eyeUvRadius(float theta)
{
    theta = std::min(std::max(theta, 0.0f), kPi);
    if (theta <= kFrontAngle) return kFrontRadius * theta / kFrontAngle;
    return kFrontRadius + (1.0f - kFrontRadius) * (theta - kFrontAngle) / (kPi - kFrontAngle);
}
float eyePupilRadius() { return eyeUvRadius(kPupilAngle); }
float eyeIrisRadius() { return eyeUvRadius(kIrisAngle); }

EyeSphere makeEyeSphere(int rings, int segments)
{
    rings = std::max(rings, 3);
    segments = std::max(segments, 3);
    EyeSphere s;
    auto addVertex = [&](float theta, float phi) {
        // theta from the -Y pole
        const float sy = -std::cos(theta), r = std::sin(theta);
        s.positions.push_back(r * std::cos(phi));
        s.positions.push_back(sy);
        s.positions.push_back(r * std::sin(phi));
        const float ur = eyeUvRadius(theta);
        s.uvs.push_back(0.5f + 0.5f * ur * std::cos(phi));
        s.uvs.push_back(0.5f + 0.5f * ur * std::sin(phi));
    };
    addVertex(0.0f, 0.0f);   // iris pole
    for (int i = 1; i < rings; ++i)
        for (int j = 0; j < segments; ++j) addVertex(kPi * float(i) / float(rings), 2.0f * kPi * float(j) / float(segments));
    addVertex(kPi, 0.0f);    // back pole
    const uint32_t back = uint32_t(s.positions.size() / 3 - 1);
    auto ring = [&](int i, int j) { return uint32_t(1 + (i - 1) * segments + (j % segments)); };
    // counter-clockwise seen from outside (pinned by EyeSphere.TrianglesFaceOutward)
    for (int j = 0; j < segments; ++j) { s.indices.push_back(0); s.indices.push_back(ring(1, j)); s.indices.push_back(ring(1, j + 1)); }
    for (int i = 1; i + 1 < rings; ++i)
        for (int j = 0; j < segments; ++j) {
            const uint32_t a = ring(i, j), b = ring(i, j + 1), c = ring(i + 1, j), d = ring(i + 1, j + 1);
            s.indices.insert(s.indices.end(), {a, c, b, b, c, d});
        }
    for (int j = 0; j < segments; ++j) { s.indices.push_back(back); s.indices.push_back(ring(rings - 1, j + 1)); s.indices.push_back(ring(rings - 1, j)); }
    return s;
}

QImage bakeEyeTexture(const Rgb& iris, int size, uint32_t seed)
{
    size = std::max(size, 32);
    QImage img(size, size, QImage::Format_RGB32);
    const float rIris = eyeIrisRadius(), rPupil = eyePupilRadius();
    const V3 base{iris[0], iris[1], iris[2]};
    const V3 inner = lerp3(base, V3{std::min(1.0f, base[0] * 1.35f + 0.08f), std::min(1.0f, base[1] * 1.25f + 0.05f), std::min(1.0f, base[2] * 1.05f + 0.01f)}, 0.8f);
    const V3 outer = scale3(base, 0.62f);
    const V3 sclera{0.93f, 0.92f, 0.90f};
    for (int y = 0; y < size; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < size; ++x) {
            const float px = (float(x) + 0.5f) / float(size) * 2.0f - 1.0f;
            const float py = (float(y) + 0.5f) / float(size) * 2.0f - 1.0f;
            const float rho = std::sqrt(px * px + py * py);
            const float cs = rho > 1e-5f ? px / rho : 1.0f, sn = rho > 1e-5f ? py / rho : 0.0f;   // the angle as cos/sin: seamless noise around the iris
            // sclera: off-white, a little shading and faint red veins away from the iris
            V3 col = scale3(sclera, 0.94f + 0.06f * fbm(px * 6.0f, py * 6.0f, 0.5f, seed + 7u, 3));
            const float veinNoise = fbm(cs * 5.0f + rho * 9.0f, sn * 5.0f + rho * 9.0f, rho * 14.0f, seed + 31u, 3);
            const float vein = std::pow(clamp01(1.0f - std::fabs(veinNoise * 2.0f - 1.0f) * 9.0f), 2.0f) * smoothstep(rIris * 1.25f, rIris * 2.2f, rho);
            col = lerp3(col, V3{0.78f, 0.38f, 0.36f}, 0.30f * vein);
            col = scale3(col, 1.0f - 0.10f * smoothstep(0.55f, 0.95f, rho));   // the back of the ball is shaded
            if (rho < rIris * 1.04f) {
                const float t = rho / rIris;   // 0 at the pupil centre, 1 at the limbus
                // iris: radial fibres (noise stretched along the radius), a pale collarette ring, darker crypts, a dark limbal ring
                const float fibre = fbm(cs * 22.0f, sn * 22.0f, rho * 5.0f, seed + 1u, 4);
                const float streak = fbm(cs * 60.0f, sn * 60.0f, rho * 2.0f, seed + 2u, 2);
                const float crypts = smoothstep(0.62f, 0.78f, fbm(cs * 7.0f + rho * 6.0f, sn * 7.0f + rho * 6.0f, rho * 7.0f, seed + 3u, 3));
                const float tone = clamp01(smoothstep(0.30f, 0.62f, t));   // pupil side -> rim side
                V3 ir = lerp3(inner, outer, std::pow(tone, 0.9f));
                float shade = 0.62f + 0.75f * fibre * 0.7f + 0.35f * streak * 0.5f;
                shade *= 1.0f - 0.35f * crypts;
                const float collarette = std::exp(-std::pow((t - 0.42f) / 0.07f, 2.0f));
                ir = lerp3(ir, V3{std::min(1.0f, base[0] * 1.5f + 0.12f), std::min(1.0f, base[1] * 1.4f + 0.09f), std::min(1.0f, base[2] * 1.15f + 0.03f)}, 0.45f * collarette);
                ir = scale3(ir, shade);
                ir = scale3(ir, 1.0f - 0.65f * smoothstep(0.80f, 0.99f, t));   // limbal ring
                const float edge = smoothstep(1.0f, 1.04f, t);                  // anti-aliased limbus
                col = lerp3(ir, col, edge);
                // pupil
                const float pupil = 1.0f - smoothstep(rPupil - 0.006f, rPupil + 0.006f, rho);
                col = lerp3(col, V3{0.015f, 0.015f, 0.018f}, pupil);
            }
            row[x] = qRgb(int(std::lround(clamp01(col[0]) * 255.0f)), int(std::lround(clamp01(col[1]) * 255.0f)), int(std::lround(clamp01(col[2]) * 255.0f)));
        }
    }
    return img;
}

QByteArray bakeEyeTexturePng(const Rgb& iris, int size, uint32_t seed)
{
    const QImage img = bakeEyeTexture(iris, size, seed);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    if (!img.save(&buf, "PNG")) return QByteArray();
    return bytes;
}

}  // namespace CharacterGen
