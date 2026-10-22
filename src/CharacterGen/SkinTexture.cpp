#include "SkinTexture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace CharacterGen {

namespace {

uint32_t hash3(int x, int y, int z, uint32_t seed)
{
    uint32_t h = seed * 0x9E3779B1u;
    h ^= uint32_t(x) * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h ^= uint32_t(y) * 0xC2B2AE35u;
    h = (h << 11) | (h >> 21);
    h ^= uint32_t(z) * 0x27D4EB2Fu;
    h *= 0x165667B1u;
    h ^= h >> 15;
    h *= 0x85EBCA77u;
    h ^= h >> 13;
    return h;
}

float unitHash(int x, int y, int z, uint32_t seed) { return float(hash3(x, y, z, seed) & 0xFFFFFFu) / float(0x1000000); }

float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

/// Value noise in [-1, 1].
float valueNoise(const std::array<float, 3>& p, uint32_t seed)
{
    const int x0 = int(std::floor(p[0]));
    const int y0 = int(std::floor(p[1]));
    const int z0 = int(std::floor(p[2]));
    const float fx = smooth(p[0] - float(x0));
    const float fy = smooth(p[1] - float(y0));
    const float fz = smooth(p[2] - float(z0));
    auto corner = [&](int dx, int dy, int dz) { return unitHash(x0 + dx, y0 + dy, z0 + dz, seed) * 2.0f - 1.0f; };
    const float x00 = corner(0, 0, 0) + (corner(1, 0, 0) - corner(0, 0, 0)) * fx;
    const float x10 = corner(0, 1, 0) + (corner(1, 1, 0) - corner(0, 1, 0)) * fx;
    const float x01 = corner(0, 0, 1) + (corner(1, 0, 1) - corner(0, 0, 1)) * fx;
    const float x11 = corner(0, 1, 1) + (corner(1, 1, 1) - corner(0, 1, 1)) * fx;
    const float y0v = x00 + (x10 - x00) * fy;
    const float y1v = x01 + (x11 - x01) * fy;
    return y0v + (y1v - y0v) * fz;
}

/// Fractal noise in roughly [-1, 1]; `frequency` in cycles per metre.
float fbm(const std::array<float, 3>& p, float frequency, int octaves, uint32_t seed)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    float norm = 0.0f;
    float f = frequency;
    for (int o = 0; o < octaves; ++o) {
        sum += amplitude * valueNoise({p[0] * f, p[1] * f, p[2] * f}, seed + uint32_t(o) * 101u);
        norm += amplitude;
        amplitude *= 0.5f;
        f *= 2.0f;
    }
    return sum / norm;
}

/// 1 inside a freckle, fading out; cells of `cellSize` metres, each holding a freckle with the given probability.
float freckleMask(const std::array<float, 3>& p, float cellSize, float probability, uint32_t seed)
{
    const std::array<float, 3> q = {p[0] / cellSize, p[1] / cellSize, p[2] / cellSize};
    const int cx = int(std::floor(q[0]));
    const int cy = int(std::floor(q[1]));
    const int cz = int(std::floor(q[2]));
    float best = 0.0f;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int x = cx + dx;
                const int y = cy + dy;
                const int z = cz + dz;
                if (unitHash(x, y, z, seed) > probability) continue;
                const float px = float(x) + unitHash(x, y, z, seed + 1u);
                const float py = float(y) + unitHash(x, y, z, seed + 2u);
                const float pz = float(z) + unitHash(x, y, z, seed + 3u);
                const float radius = 0.12f + 0.16f * unitHash(x, y, z, seed + 4u);   // fraction of the cell
                const float d = std::sqrt((q[0] - px) * (q[0] - px) + (q[1] - py) * (q[1] - py) + (q[2] - pz) * (q[2] - pz));
                best = std::max(best, 1.0f - smooth(std::clamp(d / radius, 0.0f, 1.0f)));
            }
    return best;
}

float luminance(const Rgb& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

Rgb scaled(const Rgb& c, float r, float g, float b) { return {std::clamp(c[0] * r, 0.0f, 1.0f), std::clamp(c[1] * g, 0.0f, 1.0f), std::clamp(c[2] * b, 0.0f, 1.0f)}; }

Rgb mix(const Rgb& a, const Rgb& b, float t) { return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t}; }

struct UvVertex {
    float x = 0, y = 0;              // texel coordinates
    std::array<float, 3> position{};
};

void rasterise(const UvVertex& a, const UvVertex& b, const UvVertex& c, int size, std::vector<std::array<float, 3>>& surface,
               std::vector<char>& covered)
{
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-9f) return;
    const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
    const int x1 = std::min(size - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
    const int y1 = std::min(size - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float px = float(x) + 0.5f;
            const float py = float(y) + 0.5f;
            const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
            const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
            const float w2 = 1.0f - w0 - w1;
            const float eps = -0.02f;
            if (w0 < eps || w1 < eps || w2 < eps) continue;
            std::array<float, 3> p{};
            for (size_t k = 0; k < 3; ++k) p[k] = a.position[k] * w0 + b.position[k] * w1 + c.position[k] * w2;
            const size_t i = size_t(y) * size_t(size) + size_t(x);
            surface[i] = p;
            covered[i] = 1;
        }
}

/// Texels no triangle covered take the average of their covered neighbours, a few passes outwards (so filtering across seams
/// does not pull in the background).
void dilate(QImage& image, std::vector<char>& covered, int passes)
{
    const int size = image.width();
    for (int pass = 0; pass < passes; ++pass) {
        std::vector<char> next = covered;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                if (covered[size_t(y) * size_t(size) + size_t(x)]) continue;
                int r = 0;
                int g = 0;
                int b = 0;
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx;
                        const int yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= size || yy >= size || !covered[size_t(yy) * size_t(size) + size_t(xx)]) continue;
                        const QRgb c = image.pixel(xx, yy);
                        r += qRed(c);
                        g += qGreen(c);
                        b += qBlue(c);
                        ++n;
                    }
                if (n == 0) continue;
                image.setPixel(x, y, qRgb(r / n, g / n, b / n));
                next[size_t(y) * size_t(size) + size_t(x)] = 1;
            }
        covered.swap(next);
    }
}

}  // namespace

Rgb lipColourFor(const Rgb& skin)
{
    const float fair = std::clamp((luminance(skin) - 0.30f) / 0.45f, 0.0f, 1.0f);
    const Rgb pink = {0.70f, 0.40f, 0.40f};
    const Rgb warm = scaled(skin, 0.92f, 0.64f, 0.62f);
    return mix(scaled(skin, 0.80f, 0.50f, 0.50f), mix(warm, pink, 0.4f * fair), 0.8f);
}

float lipMask(const std::array<float, 3>& p, const SkinLips& lips)
{
    if (!lips.valid) return 0.0f;
    const float w = lips.halfWidth;
    const float scale = w / 0.026f;                            // a 52 mm wide mouth is the reference
    const float dx = p[0] - lips.centre[0];
    const float s = std::fabs(dx) / w;
    if (s >= 1.0f) return 0.0f;
    const float dz = (p[2] - lips.centre[2]) * lips.forward;   // > 0 in front of the seam
    if (dz < -0.030f * scale || dz > 0.020f * scale) return 0.0f;
    const float line = lips.centre[1] - 0.0028f * scale * s * s;   // the corners of the mouth sit a little lower
    const float dy = p[1] - line;
    const float edge = 1.0f - s * s;
    const float upper = 0.0085f * scale * std::pow(edge, 0.55f);
    const float lower = 0.0112f * scale * std::pow(edge, 0.60f);
    const float inside = dy >= 0.0f ? upper - dy : lower + dy;   // distance to the lip border, metres
    const float side = (1.0f - s) * w * 0.6f;
    const float feather = 0.0016f * scale;
    return smooth(std::clamp(std::min(inside, side) / feather, 0.0f, 1.0f));
}

Rgb skinColourAt(const std::array<float, 3>& p, const Rgb& skin, const SkinTextureOptions& o)
{
    // large-scale tone: patches a few centimetres across, brighter / darker
    const float tone = fbm(p, 9.0f, 3, o.seed);
    Rgb c = scaled(skin, 1.0f + 0.07f * tone, 1.0f + 0.06f * tone, 1.0f + 0.05f * tone);

    // blush: capillary redness in patches of ~8 cm
    const float blushField = fbm(p, 14.0f, 2, o.seed + 17u);
    const float blush = std::clamp(blushField * 1.6f + 0.15f, 0.0f, 1.0f);
    c = mix(c, scaled(skin, 1.10f, 0.78f, 0.74f), 0.22f * blush);

    // freckles: more on fairer skin
    const float fair = std::clamp((luminance(skin) - 0.35f) / 0.4f, 0.0f, 1.0f);
    const float density = o.freckles >= 0.0f ? o.freckles : 0.04f + 0.30f * fair * fair;
    if (density > 0.0f) {
        const float f = freckleMask(p, 0.012f, density, o.seed + 31u) * (0.6f + 0.4f * blush);
        c = mix(c, scaled(skin, 0.74f, 0.62f, 0.52f), 0.55f * f);
    }

    // lips: warm colour with fine vertical creases and a darker line along the seam
    const float lip = lipMask(p, o.lips);
    if (lip > 0.0f) {
        const float crease = valueNoise({p[0] * 1100.0f, p[1] * 90.0f, p[2] * 90.0f}, o.seed + 97u);
        Rgb lc = scaled(lipColourFor(skin), 1.0f + 0.07f * crease, 1.0f + 0.07f * crease, 1.0f + 0.07f * crease);
        const float seam = 1.0f - std::clamp(std::fabs(p[1] - o.lips.centre[1] + 0.0028f * (o.lips.halfWidth / 0.026f) * (p[0] - o.lips.centre[0]) * (p[0] - o.lips.centre[0]) / (o.lips.halfWidth * o.lips.halfWidth)) / 0.0006f, 0.0f, 1.0f);
        lc = scaled(lc, 1.0f - 0.28f * seam, 1.0f - 0.28f * seam, 1.0f - 0.28f * seam);
        c = mix(c, lc, 0.85f * lip);
    }

    // pores and fine grain
    if (o.poreStrength > 0.0f) {
        const float pores = valueNoise({p[0] * 420.0f, p[1] * 420.0f, p[2] * 420.0f}, o.seed + 53u);
        const float grain = valueNoise({p[0] * 900.0f, p[1] * 900.0f, p[2] * 900.0f}, o.seed + 71u);
        const float d = 1.0f - o.poreStrength * (0.030f * std::max(0.0f, -pores) + 0.018f * grain);
        c = scaled(c, d, d, d);
    }
    return c;
}

QImage bakeSkinAlbedo(const Dna::Mesh& body, const Rgb& skin, const SkinTextureOptions& options)
{
    const int size = std::clamp(options.size, 64, 8192);
    if (body.uvs.empty() || body.positions.empty() || body.faces.empty()) return QImage();
    const size_t texels = size_t(size) * size_t(size);
    std::vector<std::array<float, 3>> surface(texels);
    std::vector<char> covered(texels, 0);

    auto vertexOf = [&](uint32_t layoutIndex, UvVertex& out) {
        if (layoutIndex >= body.layouts.size()) return false;
        const Dna::Mesh::Layout& l = body.layouts[layoutIndex];
        if (size_t(l.position) * 3 + 2 >= body.positions.size() || size_t(l.uv) * 2 + 1 >= body.uvs.size()) return false;
        out.x = body.uvs[size_t(l.uv) * 2] * float(size);
        out.y = (1.0f - body.uvs[size_t(l.uv) * 2 + 1]) * float(size);   // the scene importer flips V
        for (size_t k = 0; k < 3; ++k) out.position[k] = body.positions[size_t(l.position) * 3 + k];
        return true;
    };
    for (const auto& face : body.faces) {
        for (size_t i = 1; i + 1 < face.size(); ++i) {
            UvVertex a;
            UvVertex b;
            UvVertex c;
            if (vertexOf(face[0], a) && vertexOf(face[i], b) && vertexOf(face[i + 1], c)) rasterise(a, b, c, size, surface, covered);
        }
    }

    QImage image(size, size, QImage::Format_RGB888);
    image.fill(qRgb(int(skin[0] * 255.0f), int(skin[1] * 255.0f), int(skin[2] * 255.0f)));
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const size_t i = size_t(y) * size_t(size) + size_t(x);
            if (!covered[i]) continue;
            const Rgb c = skinColourAt(surface[i], skin, options);
            image.setPixel(x, y, qRgb(int(std::lround(c[0] * 255.0f)), int(std::lround(c[1] * 255.0f)), int(std::lround(c[2] * 255.0f))));
        }
    dilate(image, covered, 4);
    return image;
}

}  // namespace CharacterGen
