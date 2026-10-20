#include "BodyTexture.h"

#include "../DNA/DnaFit.h"
#include "../QuadRemesh/QuadRemeshCore.h"

#include <QBuffer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace SamBody {

namespace {

using V3 = std::array<float, 3>;

inline V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
inline float dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline V3 normalized(V3 v)
{
    const float l = std::sqrt(dot(v, v));
    if (l > 1e-20f) { v[0] /= l; v[1] /= l; v[2] /= l; }
    return v;
}
inline V3 at(const std::vector<float>& p, size_t i) { return {p[i * 3], p[i * 3 + 1], p[i * 3 + 2]}; }

std::vector<float> vertexNormals(const std::vector<float>& pos, const std::vector<int>& tris)
{
    std::vector<float> n(pos.size(), 0.0f);
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const V3 a = at(pos, tris[t]), b = at(pos, tris[t + 1]), c = at(pos, tris[t + 2]);
        const V3 fn = cross(sub(b, a), sub(c, a));            // area weighted
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j) n[static_cast<size_t>(tris[t + k]) * 3 + j] += fn[j];
    }
    for (size_t i = 0; i + 2 < n.size(); i += 3) {
        const V3 v = normalized({n[i], n[i + 1], n[i + 2]});
        n[i] = v[0]; n[i + 1] = v[1]; n[i + 2] = v[2];
    }
    return n;
}

/// Signed volume > 0 when the winding is outward (counter-clockwise seen from outside).
double signedVolume(const std::vector<float>& pos, const std::vector<uint32_t>& idx)
{
    double v = 0;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const V3 a = at(pos, idx[t]), b = at(pos, idx[t + 1]), c = at(pos, idx[t + 2]);
        v += static_cast<double>(dot(a, cross(b, c)));
    }
    return v / 6.0;
}

struct Bounds { V3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f}; };
Bounds boundsOf(const std::vector<float>& pos)
{
    Bounds b;
    for (size_t i = 0; i + 2 < pos.size(); i += 3)
        for (int k = 0; k < 3; ++k) { b.mn[k] = std::min(b.mn[k], pos[i + k]); b.mx[k] = std::max(b.mx[k], pos[i + k]); }
    return b;
}

/// Source positions scaled to the body's height, feet on the body's feet, centred on the body in x/z, optionally turned 180 degrees.
std::vector<float> alignSource(const std::vector<float>& src, const Bounds& sb, const Bounds& bb, bool turn)
{
    const float hs = std::max(1e-6f, sb.mx[1] - sb.mn[1]), hb = bb.mx[1] - bb.mn[1];
    const float s = hb / hs;
    const float scx = 0.5f * (sb.mn[0] + sb.mx[0]), scz = 0.5f * (sb.mn[2] + sb.mx[2]);
    const float bcx = 0.5f * (bb.mn[0] + bb.mx[0]), bcz = 0.5f * (bb.mn[2] + bb.mx[2]);
    std::vector<float> out(src.size());
    for (size_t i = 0; i + 2 < src.size(); i += 3) {
        float x = (src[i] - scx) * s, z = (src[i + 2] - scz) * s;
        if (turn) { x = -x; z = -z; }
        out[i] = x + bcx;
        out[i + 1] = (src[i + 1] - sb.mn[1]) * s + bb.mn[1];
        out[i + 2] = z + bcz;
    }
    return out;
}

double meanDistance(const QuadRemesh::SurfaceLocator& loc, const std::vector<float>& pts, size_t stride)
{
    double sum = 0; size_t n = 0;
    for (size_t i = 0; i + 2 < pts.size(); i += 3 * stride) {
        int tri; float bary[3], cp[3];
        const float p[3] = {pts[i], pts[i + 1], pts[i + 2]};
        if (!loc.closest(p, tri, bary, cp)) continue;
        const float d[3] = {p[0] - cp[0], p[1] - cp[1], p[2] - cp[2]};
        sum += std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); ++n;
    }
    return n ? sum / static_cast<double>(n) : 1e30;
}

float sampleGray(const QImage& img, float u, float v)
{
    const float x = u * img.width() - 0.5f, y = v * img.height() - 0.5f;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x - x0, fy = y - y0;
    auto px = [&](int xx, int yy) {
        xx = std::clamp(xx, 0, img.width() - 1); yy = std::clamp(yy, 0, img.height() - 1);
        return static_cast<float>(qGray(img.pixel(xx, yy)));
    };
    return (px(x0, y0) * (1 - fx) + px(x0 + 1, y0) * fx) * (1 - fy) + (px(x0, y0 + 1) * (1 - fx) + px(x0 + 1, y0 + 1) * fx) * fy;
}

void sampleRgb(const QImage& img, float u, float v, float out[3])
{
    const float x = u * img.width() - 0.5f, y = v * img.height() - 0.5f;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x - x0, fy = y - y0;
    auto px = [&](int xx, int yy) {
        xx = std::clamp(xx, 0, img.width() - 1); yy = std::clamp(yy, 0, img.height() - 1);
        return img.pixel(xx, yy);
    };
    const QRgb p00 = px(x0, y0), p10 = px(x0 + 1, y0), p01 = px(x0, y0 + 1), p11 = px(x0 + 1, y0 + 1);
    auto ch = [&](int (*f)(QRgb)) {
        return (f(p00) * (1 - fx) + f(p10) * fx) * (1 - fy) + (f(p01) * (1 - fx) + f(p11) * fx) * fy;
    };
    out[0] = ch(qRed); out[1] = ch(qGreen); out[2] = ch(qBlue);
}

} // namespace

UvLayout parseUvLayout(const QByteArray& b)
{
    UvLayout u;
    if (b.size() < 8) return u;
    int32_t nt = 0, nf = 0;
    std::memcpy(&nt, b.constData(), 4); std::memcpy(&nf, b.constData() + 4, 4);
    if (nt <= 0 || nf <= 0) return u;
    const qint64 need = 8 + static_cast<qint64>(nt) * 8 + static_cast<qint64>(nf) * 12;
    if (b.size() < need) return u;
    u.texcoords.resize(static_cast<size_t>(nt) * 2);
    std::memcpy(u.texcoords.data(), b.constData() + 8, static_cast<size_t>(nt) * 8);
    u.faceUv.resize(static_cast<size_t>(nf) * 3);
    std::memcpy(u.faceUv.data(), b.constData() + 8 + static_cast<size_t>(nt) * 8, static_cast<size_t>(nf) * 12);
    for (int i : u.faceUv) if (i < 0 || i >= nt) return {};
    return u;
}

UvLayout loadUvLayout(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return parseUvLayout(f.readAll());
}

TransferResult transferTexture(const TexturedSurface& source, const std::vector<float>& bodyPos,
                               const std::vector<int>& bodyTris, const UvLayout& uv, const TransferOptions& opt)
{
    TransferResult r;
    if (source.positions.size() < 9 || source.indices.size() < 3 || source.uvs.size() != source.positions.size() / 3 * 2
        || source.albedo.isNull()) {
        r.error = QStringLiteral("the source surface needs positions, triangles, UVs and an albedo texture");
        return r;
    }
    if (bodyPos.size() < 9 || bodyTris.size() < 3 || !uv.valid() || uv.faceUv.size() != bodyTris.size()) {
        r.error = QStringLiteral("the body mesh and its UV layout do not match");
        return r;
    }
    for (uint32_t i : source.indices) if (i >= source.positions.size() / 3) { r.error = QStringLiteral("source index out of range"); return r; }
    for (int i : bodyTris) if (i < 0 || static_cast<size_t>(i) >= bodyPos.size() / 3) { r.error = QStringLiteral("body index out of range"); return r; }
    const int size = std::clamp(opt.textureSize, 64, 8192);

    const Bounds bb = boundsOf(bodyPos), sb = boundsOf(source.positions);
    const float bodyHeight = bb.mx[1] - bb.mn[1];
    if (!(bodyHeight > 1e-6f) || !(sb.mx[1] - sb.mn[1] > 1e-6f)) { r.error = QStringLiteral("degenerate mesh height"); return r; }

    // Triangle winding of the source decides which way its normals point.
    std::vector<uint32_t> srcIdx = source.indices;
    const bool inside = signedVolume(source.positions, srcIdx) < 0;

    // 1. similarity alignment, both facings tried
    std::vector<float> aligned;
    double best = 1e30;
    for (int turn = 0; turn < (opt.tryBothFacings ? 2 : 1); ++turn) {
        std::vector<float> cand = alignSource(source.positions, sb, bb, turn == 1);
        QuadRemesh::SurfaceLocator loc;
        loc.build(cand, srcIdx);
        const double d = meanDistance(loc, bodyPos, std::max<size_t>(1, bodyPos.size() / 3 / 1500));
        if (d < best) { best = d; aligned = std::move(cand); r.yawDegrees = turn == 1 ? 180.0f : 0.0f; }
    }
    QuadRemesh::SurfaceLocator loc;
    loc.build(aligned, srcIdx);

    auto srcTriNormal = [&](int t) {
        const V3 a = at(aligned, srcIdx[t * 3]), b = at(aligned, srcIdx[t * 3 + 1]), c = at(aligned, srcIdx[t * 3 + 2]);
        V3 n = normalized(cross(sub(b, a), sub(c, a)));
        if (inside) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
        return n;
    };

    // Body normals must point outward whatever the file's winding.
    std::vector<int> normalTris = bodyTris;
    {
        std::vector<uint32_t> asU(bodyTris.begin(), bodyTris.end());
        if (signedVolume(bodyPos, asU) < 0)
            for (size_t t = 0; t + 2 < normalTris.size(); t += 3) std::swap(normalTris[t + 1], normalTris[t + 2]);
    }

    // 2. coarse-to-fine wrap of the body onto the surface
    std::vector<float> cur = bodyPos;
    if (opt.nonRigid) {
        const int levels[] = {24, 60, 150, 400};
        for (int li = 0; li < 4; ++li) {
            for (int round = 0; round < 2; ++round) {
                const std::vector<float> nrm = vertexNormals(cur, normalTris);
                const float maxD = bodyHeight * (li == 0 ? 0.2f : 0.1f);
                std::vector<std::array<double, 3>> from, to;
                for (size_t v = 0; v < cur.size() / 3; ++v) {
                    const float p[3] = {cur[v * 3], cur[v * 3 + 1], cur[v * 3 + 2]};
                    int tri; float bary[3], cp[3];
                    if (!loc.closest(p, tri, bary, cp)) continue;
                    const V3 d = {cp[0] - p[0], cp[1] - p[1], cp[2] - p[2]};
                    if (std::sqrt(dot(d, d)) > maxD) continue;
                    if (dot({nrm[v * 3], nrm[v * 3 + 1], nrm[v * 3 + 2]}, srcTriNormal(tri)) < 0.2f) continue;
                    from.push_back({p[0], p[1], p[2]});
                    to.push_back({cp[0], cp[1], cp[2]});
                }
                if (from.size() < 16) { r.notes << QStringLiteral("wrap level %1: too few correspondences").arg(li); break; }
                Dna::Fit::Options fo;
                fo.maxControlPoints = levels[li];
                QString err;
                const Dna::Fit::Warp w = Dna::Fit::buildWarp(from, to, fo, &err);
                if (!w.valid) { r.notes << QStringLiteral("wrap level %1 failed: %2").arg(li).arg(err); break; }
                for (size_t v = 0; v < cur.size() / 3; ++v) {
                    const auto d = w.displacement({cur[v * 3], cur[v * 3 + 1], cur[v * 3 + 2]});
                    cur[v * 3] += static_cast<float>(d[0]); cur[v * 3 + 1] += static_cast<float>(d[1]); cur[v * 3 + 2] += static_cast<float>(d[2]);
                }
            }
        }
    }
    r.meanDistanceFraction = static_cast<float>(meanDistance(loc, cur, 1) / bodyHeight);
    const std::vector<float> curNormals = vertexNormals(cur, normalTris);

    // 3. bake: every atlas texel -> 3D point on the wrapped body -> surface point -> sampled colour
    const size_t texels = static_cast<size_t>(size) * size;
    std::vector<float> rgb(texels * 3, 0.0f), rough, metal;
    const bool haveRough = !source.roughness.isNull(), haveMetal = !source.metallic.isNull();
    if (haveRough) rough.assign(texels, 0.0f);
    if (haveMetal) metal.assign(texels, 0.0f);
    std::vector<uint8_t> state(texels, 0);   // 0 = no body face, 1 = on the body but no surface match, 2 = matched
    const QImage albedo = source.albedo.convertToFormat(QImage::Format_ARGB32);
    const QImage roughImg = haveRough ? source.roughness.convertToFormat(QImage::Format_ARGB32) : QImage();
    const QImage metalImg = haveMetal ? source.metallic.convertToFormat(QImage::Format_ARGB32) : QImage();
    const float maxD = bodyHeight * opt.maxDistanceFraction;

    size_t onBody = 0, matched = 0;
    for (size_t f = 0; f < bodyTris.size() / 3; ++f) {
        float tu[3], tv[3];
        for (int k = 0; k < 3; ++k) {
            tu[k] = uv.texcoords[static_cast<size_t>(uv.faceUv[f * 3 + k]) * 2] * size;
            tv[k] = uv.texcoords[static_cast<size_t>(uv.faceUv[f * 3 + k]) * 2 + 1] * size;
        }
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({tu[0], tu[1], tu[2]})))),
                  x1 = std::min(size - 1, static_cast<int>(std::ceil(std::max({tu[0], tu[1], tu[2]})))),
                  y0 = std::max(0, static_cast<int>(std::floor(std::min({tv[0], tv[1], tv[2]})))),
                  y1 = std::min(size - 1, static_cast<int>(std::ceil(std::max({tv[0], tv[1], tv[2]}))));
        const float den = (tv[1] - tv[2]) * (tu[0] - tu[2]) + (tu[2] - tu[1]) * (tv[0] - tv[2]);
        if (std::fabs(den) < 1e-12f) continue;
        const V3 p0 = at(cur, bodyTris[f * 3]), p1 = at(cur, bodyTris[f * 3 + 1]), p2 = at(cur, bodyTris[f * 3 + 2]);
        const V3 n0 = at(curNormals, bodyTris[f * 3]), n1 = at(curNormals, bodyTris[f * 3 + 1]), n2 = at(curNormals, bodyTris[f * 3 + 2]);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float w0 = ((tv[1] - tv[2]) * (px - tu[2]) + (tu[2] - tu[1]) * (py - tv[2])) / den;
                const float w1 = ((tv[2] - tv[0]) * (px - tu[2]) + (tu[0] - tu[2]) * (py - tv[2])) / den;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                const size_t ti = static_cast<size_t>(y) * size + x;
                if (state[ti] == 0) { state[ti] = 1; ++onBody; }
                const float p[3] = {w0 * p0[0] + w1 * p1[0] + w2 * p2[0], w0 * p0[1] + w1 * p1[1] + w2 * p2[1], w0 * p0[2] + w1 * p1[2] + w2 * p2[2]};
                int tri; float bary[3], cp[3];
                if (!loc.closest(p, tri, bary, cp)) continue;
                const V3 d = {cp[0] - p[0], cp[1] - p[1], cp[2] - p[2]};
                if (std::sqrt(dot(d, d)) > maxD) continue;
                const V3 n = normalized({w0 * n0[0] + w1 * n1[0] + w2 * n2[0], w0 * n0[1] + w1 * n1[1] + w2 * n2[1], w0 * n0[2] + w1 * n1[2] + w2 * n2[2]});
                if (dot(n, srcTriNormal(tri)) < opt.minNormalDot) continue;
                const uint32_t i0 = srcIdx[tri * 3], i1 = srcIdx[tri * 3 + 1], i2 = srcIdx[tri * 3 + 2];
                const float u = bary[0] * source.uvs[i0 * 2] + bary[1] * source.uvs[i1 * 2] + bary[2] * source.uvs[i2 * 2];
                const float v = bary[0] * source.uvs[i0 * 2 + 1] + bary[1] * source.uvs[i1 * 2 + 1] + bary[2] * source.uvs[i2 * 2 + 1];
                float c[3];
                sampleRgb(albedo, u, v, c);
                rgb[ti * 3] = c[0]; rgb[ti * 3 + 1] = c[1]; rgb[ti * 3 + 2] = c[2];
                if (haveRough) rough[ti] = sampleGray(roughImg, u, v);
                if (haveMetal) metal[ti] = sampleGray(metalImg, u, v);
                if (state[ti] != 2) { state[ti] = 2; ++matched; }
            }
    }
    r.coverage = onBody ? static_cast<float>(matched) / static_cast<float>(onBody) : 0.0f;
    if (onBody == 0) { r.error = QStringLiteral("the UV layout covers no texels"); return r; }
    if (matched == 0) {
        r.error = QStringLiteral("no texel matched the textured surface (the body could not be aligned to it; mean wrap residual %1% of height)")
                      .arg(r.meanDistanceFraction * 100.0f, 0, 'f', 1);
        return r;
    }

    // 4. fill what no surface matched (gutters, gaps) from the matched neighbours, a ring at a time
    std::vector<uint8_t> filled = state;
    for (int pass = 0; pass < opt.dilatePx; ++pass) {
        std::vector<uint8_t> next = filled;
        bool any = false;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const size_t ti = static_cast<size_t>(y) * size + x;
                if (filled[ti] == 2) continue;
                float acc[3] = {0, 0, 0}, ar = 0, am = 0; int cnt = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= size || ny >= size) continue;
                        const size_t ni = static_cast<size_t>(ny) * size + nx;
                        if (filled[ni] != 2) continue;
                        acc[0] += rgb[ni * 3]; acc[1] += rgb[ni * 3 + 1]; acc[2] += rgb[ni * 3 + 2];
                        if (haveRough) ar += rough[ni];
                        if (haveMetal) am += metal[ni];
                        ++cnt;
                    }
                if (!cnt) continue;
                rgb[ti * 3] = acc[0] / cnt; rgb[ti * 3 + 1] = acc[1] / cnt; rgb[ti * 3 + 2] = acc[2] / cnt;
                if (haveRough) rough[ti] = ar / cnt;
                if (haveMetal) metal[ti] = am / cnt;
                next[ti] = 2; any = true;
            }
        filled.swap(next);
        if (!any) break;
    }

    r.albedo = QImage(size, size, QImage::Format_RGBA8888);
    r.albedo.fill(Qt::transparent);
    if (haveRough) { r.roughness = QImage(size, size, QImage::Format_Grayscale8); r.roughness.fill(0); }
    if (haveMetal) { r.metallic = QImage(size, size, QImage::Format_Grayscale8); r.metallic.fill(0); }
    for (int y = 0; y < size; ++y) {
        uchar* line = r.albedo.scanLine(y);
        for (int x = 0; x < size; ++x) {
            const size_t ti = static_cast<size_t>(y) * size + x;
            if (filled[ti] != 2) continue;
            line[x * 4] = static_cast<uchar>(std::clamp(rgb[ti * 3] + 0.5f, 0.0f, 255.0f));
            line[x * 4 + 1] = static_cast<uchar>(std::clamp(rgb[ti * 3 + 1] + 0.5f, 0.0f, 255.0f));
            line[x * 4 + 2] = static_cast<uchar>(std::clamp(rgb[ti * 3 + 2] + 0.5f, 0.0f, 255.0f));
            line[x * 4 + 3] = 255;
            if (haveRough) r.roughness.scanLine(y)[x] = static_cast<uchar>(std::clamp(rough[ti] + 0.5f, 0.0f, 255.0f));
            if (haveMetal) r.metallic.scanLine(y)[x] = static_cast<uchar>(std::clamp(metal[ti] + 0.5f, 0.0f, 255.0f));
        }
    }
    r.ok = true;
    return r;
}

QByteArray toTexturedObj(const std::vector<float>& pos, const std::vector<int>& tris, const UvLayout& uv,
                         const QString& mtlFileName, bool facingMinusZ)
{
    QByteArray o;
    o += "# SAM 3D Body / MHR textured body (metres)\n";
    if (!mtlFileName.isEmpty()) o += "mtllib " + mtlFileName.toUtf8() + "\nusemtl body\n";
    const float sx = facingMinusZ ? -1.0f : 1.0f;
    for (size_t i = 0; i + 2 < pos.size(); i += 3)
        o += "v " + QByteArray::number(pos[i] * sx, 'g', 9) + ' ' + QByteArray::number(pos[i + 1], 'g', 9) + ' '
             + QByteArray::number(pos[i + 2] * sx, 'g', 9) + '\n';
    for (size_t i = 0; i + 1 < uv.texcoords.size(); i += 2)   // atlas rows are v-down, OBJ is v-up
        o += "vt " + QByteArray::number(uv.texcoords[i], 'g', 9) + ' ' + QByteArray::number(1.0f - uv.texcoords[i + 1], 'g', 9) + '\n';
    for (size_t f = 0; f < tris.size() / 3; ++f) {
        o += "f";
        for (int k = 0; k < 3; ++k)
            o += ' ' + QByteArray::number(tris[f * 3 + k] + 1) + '/' + QByteArray::number(uv.faceUv[f * 3 + k] + 1);
        o += '\n';
    }
    return o;
}


QByteArray toTexturedGlb(const std::vector<float>& pos, const std::vector<int>& tris, const UvLayout& uv,
                         const QImage& albedo, const QImage& roughness, const QImage& metallic, bool facingMinusZ)
{
    if (tris.empty() || uv.faceUv.size() != tris.size() || albedo.isNull()) return {};
    const float sx = facingMinusZ ? -1.0f : 1.0f;

    // UV-split vertices: one per unique (position, uv) pair.
    std::vector<float> P, T, N;
    std::vector<uint32_t> I;
    QHash<quint64, uint32_t> remap;
    std::vector<int> srcVertex;
    for (size_t k = 0; k < tris.size(); ++k) {
        const quint64 key = (static_cast<quint64>(tris[k]) << 32) | static_cast<quint32>(uv.faceUv[k]);
        auto it = remap.find(key);
        if (it == remap.end()) {
            it = remap.insert(key, static_cast<uint32_t>(P.size() / 3));
            P.insert(P.end(), {pos[tris[k] * 3] * sx, pos[tris[k] * 3 + 1], pos[tris[k] * 3 + 2] * sx});
            T.insert(T.end(), {uv.texcoords[uv.faceUv[k] * 2], uv.texcoords[uv.faceUv[k] * 2 + 1]});
            srcVertex.push_back(tris[k]);
        }
        I.push_back(it.value());
    }
    // Smooth normals from the ORIGINAL (welded) vertices so seams shade continuously; the x/z mirror is a proper rotation.
    std::vector<float> wp(pos.size());
    for (size_t i = 0; i + 2 < pos.size(); i += 3) { wp[i] = pos[i] * sx; wp[i + 1] = pos[i + 1]; wp[i + 2] = pos[i + 2] * sx; }
    std::vector<int> nt = tris;
    {
        std::vector<uint32_t> asU(tris.begin(), tris.end());
        if (signedVolume(wp, asU) < 0) for (size_t t = 0; t + 2 < nt.size(); t += 3) std::swap(nt[t + 1], nt[t + 2]);
    }
    const std::vector<float> wn = vertexNormals(wp, nt);
    for (int sv : srcVertex) N.insert(N.end(), {wn[sv * 3], wn[sv * 3 + 1], wn[sv * 3 + 2]});
    // glTF winding is counter-clockwise: make the triangle order agree with the outward normals.
    {
        std::vector<uint32_t> asU(tris.begin(), tris.end());
        if (signedVolume(wp, asU) < 0) for (size_t t = 0; t + 2 < I.size(); t += 3) std::swap(I[t + 1], I[t + 2]);
    }

    auto png = [](const QImage& img) { QByteArray b; QBuffer buf(&b); buf.open(QIODevice::WriteOnly); img.save(&buf, "PNG"); return b; };
    QByteArray albedoPng = png(albedo.convertToFormat(QImage::Format_RGBA8888));
    QByteArray mrPng;
    if (!roughness.isNull() || !metallic.isNull()) {
        const int w = !roughness.isNull() ? roughness.width() : metallic.width(), h = !roughness.isNull() ? roughness.height() : metallic.height();
        QImage mr(w, h, QImage::Format_RGBA8888);
        const QImage r = roughness.isNull() ? QImage() : roughness.scaled(w, h).convertToFormat(QImage::Format_Grayscale8);
        const QImage m = metallic.isNull() ? QImage() : metallic.scaled(w, h).convertToFormat(QImage::Format_Grayscale8);
        for (int y = 0; y < h; ++y) {
            uchar* d = mr.scanLine(y);
            for (int x = 0; x < w; ++x) {
                d[x * 4] = 255;
                d[x * 4 + 1] = r.isNull() ? 255 : r.constScanLine(y)[x];
                d[x * 4 + 2] = m.isNull() ? 0 : m.constScanLine(y)[x];
                d[x * 4 + 3] = 255;
            }
        }
        mrPng = png(mr);
    }

    QByteArray bin;
    auto align4 = [&]() { while (bin.size() % 4) bin.append('\0'); };
    auto push = [&](const void* d, int n) { align4(); const int off = bin.size(); bin.append(static_cast<const char*>(d), n); return off; };
    const int offP = push(P.data(), static_cast<int>(P.size() * 4)), offN = push(N.data(), static_cast<int>(N.size() * 4)),
              offT = push(T.data(), static_cast<int>(T.size() * 4)), offI = push(I.data(), static_cast<int>(I.size() * 4)),
              offA = push(albedoPng.constData(), albedoPng.size());
    const int offM = mrPng.isEmpty() ? -1 : push(mrPng.constData(), mrPng.size());
    align4();

    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i + 2 < P.size(); i += 3) for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], P[i + k]); mx[k] = std::max(mx[k], P[i + k]); }

    QJsonArray views{
        QJsonObject{{"buffer", 0}, {"byteOffset", offP}, {"byteLength", int(P.size() * 4)}, {"target", 34962}},
        QJsonObject{{"buffer", 0}, {"byteOffset", offN}, {"byteLength", int(N.size() * 4)}, {"target", 34962}},
        QJsonObject{{"buffer", 0}, {"byteOffset", offT}, {"byteLength", int(T.size() * 4)}, {"target", 34962}},
        QJsonObject{{"buffer", 0}, {"byteOffset", offI}, {"byteLength", int(I.size() * 4)}, {"target", 34963}},
        QJsonObject{{"buffer", 0}, {"byteOffset", offA}, {"byteLength", albedoPng.size()}}};
    if (offM >= 0) views.append(QJsonObject{{"buffer", 0}, {"byteOffset", offM}, {"byteLength", mrPng.size()}});
    QJsonArray images{QJsonObject{{"bufferView", 4}, {"mimeType", "image/png"}}};
    QJsonArray textures{QJsonObject{{"source", 0}, {"sampler", 0}}};
    QJsonObject pbr{{"baseColorTexture", QJsonObject{{"index", 0}}}, {"metallicFactor", offM >= 0 ? 1.0 : 0.0}, {"roughnessFactor", 1.0}};
    if (offM >= 0) {
        images.append(QJsonObject{{"bufferView", 5}, {"mimeType", "image/png"}});
        textures.append(QJsonObject{{"source", 1}, {"sampler", 0}});
        pbr["metallicRoughnessTexture"] = QJsonObject{{"index", 1}};
    }
    const int nv = static_cast<int>(P.size() / 3);
    QJsonObject root{
        {"asset", QJsonObject{{"version", "2.0"}, {"generator", "QtMeshEditor SAM 3D Body"}}},
        {"scene", 0}, {"scenes", QJsonArray{QJsonObject{{"nodes", QJsonArray{0}}}}},
        {"nodes", QJsonArray{QJsonObject{{"mesh", 0}, {"name", "body"}}}},
        {"meshes", QJsonArray{QJsonObject{{"name", "body"}, {"primitives", QJsonArray{QJsonObject{
            {"attributes", QJsonObject{{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}}, {"indices", 3}, {"material", 0}}}}}}},
        {"materials", QJsonArray{QJsonObject{{"name", "body"}, {"pbrMetallicRoughness", pbr}}}},
        {"textures", textures}, {"images", images},
        {"samplers", QJsonArray{QJsonObject{{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 33071}, {"wrapT", 33071}}}},
        {"accessors", QJsonArray{
            QJsonObject{{"bufferView", 0}, {"componentType", 5126}, {"count", nv}, {"type", "VEC3"},
                        {"min", QJsonArray{mn[0], mn[1], mn[2]}}, {"max", QJsonArray{mx[0], mx[1], mx[2]}}},
            QJsonObject{{"bufferView", 1}, {"componentType", 5126}, {"count", nv}, {"type", "VEC3"}},
            QJsonObject{{"bufferView", 2}, {"componentType", 5126}, {"count", nv}, {"type", "VEC2"}},
            QJsonObject{{"bufferView", 3}, {"componentType", 5125}, {"count", int(I.size())}, {"type", "SCALAR"}}}},
        {"bufferViews", views}, {"buffers", QJsonArray{QJsonObject{{"byteLength", bin.size()}}}}};
    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    while (json.size() % 4) json.append(' ');
    auto le32 = [](quint32 v) { QByteArray b(4, 0); for (int i = 0; i < 4; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xff); return b; };
    QByteArray glb = QByteArray("glTF") + le32(2) + le32(12 + 8 + json.size() + 8 + bin.size())
                     + le32(json.size()) + QByteArray("JSON") + json + le32(bin.size()) + QByteArray("BIN\0", 4) + bin;
    return glb;
}

QByteArray toMtl(const QString& name, const QString& diffuse, const QString& roughness, const QString& metallic)
{
    QByteArray m = "newmtl " + name.toUtf8() + "\nKa 1 1 1\nKd 1 1 1\nKs 0.1 0.1 0.1\nNs 30\n";
    if (!diffuse.isEmpty()) m += "map_Kd " + diffuse.toUtf8() + "\n";
    if (!roughness.isEmpty()) m += "map_Pr " + roughness.toUtf8() + "\n";
    if (!metallic.isEmpty()) m += "map_Pm " + metallic.toUtf8() + "\n";
    return m;
}

} // namespace SamBody
