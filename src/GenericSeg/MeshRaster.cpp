#include "MeshRaster.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GenericSeg {

OrthoCamera OrthoCamera::lookingFrom(const Vec3& toCamera, const Vec3& upHint)
{
    OrthoCamera c;
    c.toCamera = normalized(toCamera);
    Vec3 hint = normalized(upHint);
    if (std::fabs(dot(hint, c.toCamera)) > 0.95f)
        hint = std::fabs(c.toCamera.z) < 0.9f ? Vec3(0, 0, 1) : Vec3(1, 0, 0);
    // Right-handed image: right = up x toCamera.
    c.right = normalized(cross(hint, c.toCamera));
    c.up = cross(c.toCamera, c.right);
    return c;
}

std::vector<int> Raster::pixelsPerFace(int faceCount) const
{
    std::vector<int> n(std::max(0, faceCount), 0);
    for (int f : face)
        if (f >= 0 && f < faceCount) ++n[f];
    return n;
}

Raster rasterize(const SegMesh& mesh, const OrthoCamera& cam, int size)
{
    Raster r;
    r.size = std::max(1, size);
    const size_t n = size_t(r.size) * r.size;
    r.face.assign(n, -1);
    r.depth.assign(n, std::numeric_limits<float>::infinity());
    r.b1.assign(n, 0.0f);
    r.b2.assign(n, 0.0f);

    const float pxPerUnit = 0.5f * r.size / cam.halfExtent;
    struct P { float x, y, z; };
    auto project = [&](const Vec3& p) {
        return P{(dot(p, cam.right) + cam.halfExtent) * pxPerUnit,
                 (cam.halfExtent - dot(p, cam.up)) * pxPerUnit,
                 -dot(p, cam.toCamera)};
    };
    const int faceCount = mesh.faceCount();
    for (int f = 0; f < faceCount; ++f) {
        const P a = project(mesh.positions[mesh.indices[3 * f]]);
        const P b = project(mesh.positions[mesh.indices[3 * f + 1]]);
        const P c = project(mesh.positions[mesh.indices[3 * f + 2]]);
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-12f) continue;   // edge-on or degenerate
        const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(r.size - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(r.size - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
        const float inv = 1.0f / area;
        for (int y = y0; y <= y1; ++y) {
            const float py = y + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f;
                // Barycentrics (both windings: no culling, both sides render).
                const float w1 = ((px - a.x) * (c.y - a.y) - (py - a.y) * (c.x - a.x)) * inv;
                const float w2 = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * inv;
                const float w0 = 1.0f - w1 - w2;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                const float z = w0 * a.z + w1 * b.z + w2 * c.z;
                const size_t i = size_t(y) * r.size + x;
                if (z < r.depth[i]) {
                    r.depth[i] = z;
                    r.face[i] = f;
                    r.b1[i] = w1;
                    r.b2[i] = w2;
                }
            }
        }
    }
    return r;
}

std::vector<Vec3> viewDirections(int count)
{
    std::vector<Vec3> dirs;
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    const std::vector<Vec3> ico = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
        {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
        {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    if (count == 12) {
        for (const Vec3& v : ico) dirs.push_back(normalized(v));
        return dirs;
    }
    if (count == 20) {
        static const int faces[20][3] = {
            {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
            {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
            {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
            {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
        for (const auto& f : faces)
            dirs.push_back(normalized(ico[f[0]] + ico[f[1]] + ico[f[2]]));
        return dirs;
    }
    const int n = std::max(1, count);
    const float golden = 3.14159265f * (3.0f - std::sqrt(5.0f));
    for (int i = 0; i < n; ++i) {
        const float y = n == 1 ? 0.0f : 1.0f - 2.0f * (i + 0.5f) / n;
        const float rr = std::sqrt(std::max(0.0f, 1.0f - y * y));
        dirs.push_back({std::cos(golden * i) * rr, y, std::sin(golden * i) * rr});
    }
    return dirs;
}

namespace {
QImage blankImage(int size)
{
    QImage img(size, size, QImage::Format_RGB32);
    img.fill(Qt::black);
    return img;
}

int toByte(float v) { return std::clamp(int(std::lround(v * 255.0f)), 0, 255); }

// Perceptually ordered ramp (dark blue -> teal -> yellow), 5 stops.
QRgb ramp(float t)
{
    static const float stops[5][3] = {
        {0.20f, 0.05f, 0.45f}, {0.15f, 0.35f, 0.60f}, {0.15f, 0.60f, 0.55f},
        {0.50f, 0.80f, 0.30f}, {0.99f, 0.90f, 0.15f}};
    t = std::clamp(t, 0.0f, 1.0f) * 4.0f;
    const int i = std::min(3, int(t));
    const float f = t - i;
    float c[3];
    for (int k = 0; k < 3; ++k) c[k] = stops[i][k] * (1 - f) + stops[i + 1][k] * f;
    return qRgb(toByte(c[0]), toByte(c[1]), toByte(c[2]));
}
} // namespace

QImage shadeNormals(const Raster& r, const SegMesh& mesh)
{
    QImage img = blankImage(r.size);
    for (int y = 0; y < r.size; ++y) {
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < r.size; ++x) {
            const size_t i = size_t(y) * r.size + x;
            const int f = r.face[i];
            if (f < 0) continue;
            const float w1 = r.b1[i], w2 = r.b2[i], w0 = 1.0f - w1 - w2;
            const Vec3 n = normalized(mesh.weldedNormals[mesh.weldedIndices[3 * f]] * w0
                                      + mesh.weldedNormals[mesh.weldedIndices[3 * f + 1]] * w1
                                      + mesh.weldedNormals[mesh.weldedIndices[3 * f + 2]] * w2);
            line[x] = qRgb(toByte(n.x * 0.5f + 0.5f), toByte(n.y * 0.5f + 0.5f), toByte(n.z * 0.5f + 0.5f));
        }
    }
    return img;
}

QImage shadeFaceScalar(const Raster& r, const std::vector<float>& value)
{
    QImage img = blankImage(r.size);
    for (int y = 0; y < r.size; ++y) {
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < r.size; ++x) {
            const int f = r.face[size_t(y) * r.size + x];
            if (f >= 0 && f < int(value.size())) line[x] = ramp(value[f]);
        }
    }
    return img;
}

QImage shadeMatte(const Raster& r, const SegMesh& mesh, const OrthoCamera& cam)
{
    QImage img = blankImage(r.size);
    for (int y = 0; y < r.size; ++y) {
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < r.size; ++x) {
            const int f = r.face[size_t(y) * r.size + x];
            if (f < 0) continue;
            // Two-sided: a back face lights like a front face.
            const float l = std::fabs(dot(mesh.faceNormals[f], cam.toCamera));
            const int g = toByte(0.15f + 0.8f * l);
            line[x] = qRgb(g, g, g);
        }
    }
    return img;
}

} // namespace GenericSeg
