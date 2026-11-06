#include "PhotoAppearance.h"

#include "../FaceRig/FaceLandmarkDetector.h"
#include "../ImageTo3D/BackgroundRemover.h"
#include "../Mocap/FaceCapPose.h"

#include <QFile>

#include <QPainter>
#include <QPainterPath>

#include <cstdio>
#include <algorithm>
#include <cmath>

namespace CharacterGen {
namespace PhotoAppearance {

namespace {

float srgbToLin(float c)
{
    c /= 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float labF(float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.0f / 116.0f; }
float labInvF(float t) { return t > 0.2069f ? t * t * t : (t - 16.0f / 116.0f) / 7.787f; }
float linToSrgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
float dist(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    const float dl = a[0] - b[0], da = a[1] - b[1], db = a[2] - b[2];
    return std::sqrt(dl * dl + da * da + db * db);
}
float median(std::vector<float> v)
{
    if (v.empty()) return 0.0f;
    std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
    return v[v.size() / 2];
}
std::array<float, 3> labMedian(const std::vector<std::array<float, 3>>& labs)
{
    std::vector<float> l, a, b;
    for (const auto& x : labs) { l.push_back(x[0]); a.push_back(x[1]); b.push_back(x[2]); }
    return {median(l), median(a), median(b)};
}
Rgb labToRgb(const std::array<float, 3>& lab)
{
    const float fy = (lab[0] + 16.0f) / 116.0f, fx = fy + lab[1] / 500.0f, fz = fy - lab[2] / 200.0f;
    const float X = 0.95047f * labInvF(fx), Y = labInvF(fy), Z = 1.08883f * labInvF(fz);
    return {linToSrgb(3.2406f * X - 1.5372f * Y - 0.4986f * Z), linToSrgb(-0.9689f * X + 1.8758f * Y + 0.0415f * Z),
            linToSrgb(0.0557f * X - 0.2040f * Y + 1.0570f * Z)};
}

// MediaPipe Face Mesh canonical landmark groups.
const int kOval[] = {10, 338, 297, 332, 284, 251, 389, 356, 454, 323, 361, 288, 397, 365, 379, 378, 400, 377, 152, 148, 176, 149, 150, 136, 172, 58, 132, 93, 234, 127, 162, 21, 54, 103, 67, 109};
const int kEyeL[] = {263, 249, 390, 373, 374, 380, 381, 382, 362, 398, 384, 385, 386, 387, 388, 466};
const int kEyeR[] = {33, 7, 163, 144, 145, 153, 154, 155, 133, 173, 157, 158, 159, 160, 161, 246};
const int kLips[] = {61, 146, 91, 181, 84, 17, 314, 405, 321, 375, 291, 409, 270, 269, 267, 0, 37, 39, 40, 185};
const int kBrowL[] = {276, 283, 282, 295, 285, 300, 293, 334, 296, 336};
const int kBrowR[] = {46, 53, 52, 65, 55, 70, 63, 105, 66, 107};

template <size_t N>
std::vector<QPointF> pts(const std::vector<std::array<float, 3>>& lm, const int (&idx)[N])
{
    std::vector<QPointF> p;
    for (int i : idx) if (size_t(i) < lm.size()) p.push_back({double(lm[size_t(i)][0]), double(lm[size_t(i)][1])});
    return p;
}

QRectF bounds(const std::vector<QPointF>& p)
{
    if (p.empty()) return {};
    double x0 = p[0].x(), x1 = x0, y0 = p[0].y(), y1 = y0;
    for (const auto& q : p) { x0 = std::min(x0, q.x()); x1 = std::max(x1, q.x()); y0 = std::min(y0, q.y()); y1 = std::max(y1, q.y()); }
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

bool on(const QImage& m, int x, int y)
{
    if (m.isNull() || x < 0 || y < 0 || x >= m.width() || y >= m.height()) return false;
    return qGray(m.pixel(x, y)) >= 128;
}

}  // namespace

std::array<float, 3> toLab(float r, float g, float b)
{
    const float R = srgbToLin(r), G = srgbToLin(g), B = srgbToLin(b);
    const float X = 0.4124f * R + 0.3576f * G + 0.1805f * B, Y = 0.2126f * R + 0.7152f * G + 0.0722f * B, Z = 0.0193f * R + 0.1192f * G + 0.9505f * B;
    const float fx = labF(X / 0.95047f), fy = labF(Y), fz = labF(Z / 1.08883f);
    return {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}

ColourEstimate robustColour(const std::vector<std::array<uint8_t, 3>>& pixels, float trimDeltaE, int minSamples)
{
    ColourEstimate e;
    std::vector<std::array<float, 3>> labs;
    labs.reserve(pixels.size());
    for (const auto& p : pixels) labs.push_back(toLab(p[0], p[1], p[2]));
    if (int(labs.size()) < minSamples) return e;
    std::array<float, 3> m = labMedian(labs);
    std::vector<std::array<float, 3>> keep;
    for (const auto& l : labs) if (dist(l, m) <= trimDeltaE) keep.push_back(l);
    if (int(keep.size()) >= minSamples / 2) m = labMedian(keep);
    std::vector<float> d;
    for (const auto& l : keep) d.push_back(dist(l, m));
    e.ok = true;
    e.colour = labToRgb(m);
    e.samples = int(keep.size());
    e.spread = median(d);
    return e;
}

ColourEstimate robustColour(const QImage& image, const QImage& mask, float trimDeltaE, int minSamples)
{
    std::vector<std::array<uint8_t, 3>> px;
    const QImage img = image.convertToFormat(QImage::Format_RGB888);
    const QImage m = mask.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        const uchar* s = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            if (x >= m.width() || y >= m.height()) continue;
            const QRgb q = m.pixel(x, y);
            if (qAlpha(q) >= 128 && qGray(q) >= 128) px.push_back({s[x * 3], s[x * 3 + 1], s[x * 3 + 2]});
        }
    }
    return robustColour(px, trimDeltaE, minSamples);
}

QImage polygonMask(int w, int h, const std::vector<QPointF>& polygon)
{
    QImage m(w, h, QImage::Format_Grayscale8);
    m.fill(0);
    if (polygon.size() < 3) return m;
    QPainter p(&m);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    QPolygonF pf;
    for (const auto& q : polygon) pf << q;
    p.drawPolygon(pf);
    return m;
}

std::vector<QPointF> scaledPolygon(const std::vector<QPointF>& polygon, double factor)
{
    if (polygon.empty()) return {};
    QPointF c(0, 0);
    for (const auto& p : polygon) c += p;
    c /= double(polygon.size());
    std::vector<QPointF> out;
    for (const auto& p : polygon) out.push_back(c + (p - c) * factor);
    return out;
}

FaceAppearance fromLandmarks(const QImage& photo, const std::vector<std::array<float, 3>>& lm, const QImage& matte)
{
    FaceAppearance fa;
    if (lm.size() < 468 || photo.isNull()) { fa.note = QStringLiteral("no face landmarks"); return fa; }
    const int W = photo.width(), H = photo.height();
    const std::vector<QPointF> oval = pts(lm, kOval);
    fa.faceFound = true;
    fa.landmarks = lm;
    fa.faceBox = bounds(oval);

    // ---- skin: the face oval shrunk away from the hairline/jaw edge, minus eyes, brows and lips (grown a little) ----
    QImage skinMask = polygonMask(W, H, scaledPolygon(oval, 0.82));
    {
        QPainter p(&skinMask);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        for (const auto& group : {pts(lm, kEyeL), pts(lm, kEyeR), pts(lm, kLips), pts(lm, kBrowL), pts(lm, kBrowR)}) {
            QPolygonF pf;
            for (const auto& q : scaledPolygon(group, 1.45)) pf << q;
            p.drawPolygon(pf);
        }
    }
    fa.skin = robustColour(photo, skinMask, 14.0f, 30);

    // ---- iris: a small disc at each iris centre (landmarks 468 and 473), pupil excluded by the median ----
    if (lm.size() >= 478) {
        std::vector<std::array<uint8_t, 3>> px;
        const QImage img = photo.convertToFormat(QImage::Format_RGB888);
        const double irisR = std::max(1.5, fa.faceBox.width() * 0.028);
        for (int c : {468, 473}) {
            const double cx = double(lm[size_t(c)][0]), cy = double(lm[size_t(c)][1]);
            for (int y = int(cy - irisR); y <= int(cy + irisR); ++y)
                for (int x = int(cx - irisR); x <= int(cx + irisR); ++x)
                    if (x >= 0 && y >= 0 && x < W && y < H && std::hypot(x - cx, y - cy) <= irisR) {
                        const uchar* s = img.constScanLine(y) + x * 3;
                        px.push_back({s[0], s[1], s[2]});
                    }
        }
        fa.iris = robustColour(px, 22.0f, 12);
    }

    // ---- hair: person pixels above and beside the face oval (down to the eye line), skin-coloured pixels removed ----
    if (!matte.isNull() && fa.skin.ok) {
        const QRectF fb = fa.faceBox;
        double eyeY = 0;
        const auto eyes = pts(lm, kEyeL);
        for (const auto& q : eyes) eyeY += q.y();
        eyeY = eyes.empty() ? fb.center().y() : eyeY / double(eyes.size());
        const double x0 = fb.left() - 0.22 * fb.width(), x1 = fb.right() + 0.22 * fb.width();
        const double y0 = fb.top() - 0.95 * fb.height(), y1 = eyeY;
        const QImage ovalMask = polygonMask(W, H, scaledPolygon(oval, 1.06));
        const QImage img = photo.convertToFormat(QImage::Format_RGB888);
        const auto skinLab = toLab(fa.skin.colour[0] * 255.0f, fa.skin.colour[1] * 255.0f, fa.skin.colour[2] * 255.0f);
        std::vector<std::array<uint8_t, 3>> px;
        for (int y = std::max(0, int(y0)); y <= std::min(H - 1, int(y1)); ++y) {
            const uchar* s = img.constScanLine(y);
            for (int x = std::max(0, int(x0)); x <= std::min(W - 1, int(x1)); ++x) {
                if (!on(matte, x, y) || on(ovalMask, x, y)) continue;
                const auto l = toLab(s[x * 3], s[x * 3 + 1], s[x * 3 + 2]);
                if (dist(l, skinLab) < 14.0f) continue;   // skin (forehead edge, ears), not hair
                px.push_back({s[x * 3], s[x * 3 + 1], s[x * 3 + 2]});
            }
        }
        fa.hair = robustColour(px, 20.0f, 60);
        if (fa.hair.ok) {
            // Length: hair-coloured person pixels hanging beside the face below the ears (a long style) vs none (short).
            double mouthY = 0;
            const auto lips = pts(lm, kLips);
            for (const auto& q : lips) mouthY += q.y();
            mouthY = lips.empty() ? fb.center().y() + 0.2 * fb.height() : mouthY / double(lips.size());
            const auto hairLab = toLab(fa.hair.colour[0] * 255.0f, fa.hair.colour[1] * 255.0f, fa.hair.colour[2] * 255.0f);
            long total = 0, hairLike = 0;
            const double yy0 = mouthY, yy1 = fb.bottom() + 0.35 * fb.height();
            for (int y = std::max(0, int(yy0)); y <= std::min(H - 1, int(yy1)); ++y) {
                const uchar* sl = img.constScanLine(y);
                for (int x = std::max(0, int(fb.left() - 0.5 * fb.width())); x <= std::min(W - 1, int(fb.right() + 0.5 * fb.width())); ++x) {
                    if (!on(matte, x, y) || on(ovalMask, x, y)) continue;
                    ++total;
                    if (dist(toLab(sl[x * 3], sl[x * 3 + 1], sl[x * 3 + 2]), hairLab) < 22.0f) ++hairLike;
                }
            }
            fa.sideHairFraction = total > 0 ? float(hairLike) / float(fb.width() * fb.height() * 0.5) : 0.0f;
            fa.hairLength = fa.sideHairFraction > 0.30f ? 3 : (fa.sideHairFraction > 0.10f ? 2 : 1);

            // ---- continuous shape of the hair silhouette (see FaceAppearance::hairTop ...) ----
            const double Wf = std::max(1.0, fb.width()), Hf = std::max(1.0, fb.height());
            auto isHair = [&](const uchar* sl, int x) {
                const auto l = toLab(sl[x * 3], sl[x * 3 + 1], sl[x * 3 + 2]);
                return dist(l, hairLab) < 26.0f && dist(l, skinLab) > 12.0f;
            };
            const int top = std::max(0, int(fb.top() - 1.3 * Hf)), bottom = std::min(H - 1, int(fb.bottom() + 2.2 * Hf));
            const int left = std::max(0, int(fb.left() - 1.2 * Wf)), right = std::min(W - 1, int(fb.right() + 1.2 * Wf));
            // top of the hair above the middle of the face
            double topY = fb.top();
            for (int y = top; y < std::min(H, int(fb.top())); ++y) {
                const uchar* sl = img.constScanLine(y);
                int n = 0;
                for (int x = std::max(0, int(fb.center().x() - 0.3 * Wf)); x <= std::min(W - 1, int(fb.center().x() + 0.3 * Wf)); ++x)
                    if (on(matte, x, y) && isHair(sl, x)) ++n;
                if (n >= std::max(3, int(0.06 * Wf))) { topY = y; break; }
            }
            fa.hairTop = float(std::clamp((fb.top() - topY) / Hf, 0.0, 1.6));
            // fullness beside the face at eye level, and the lowest strand in the side bands
            double sideSum = 0.0;
            int sideRows = 0;
            double lowest = fb.top();
            int gapRows = 0;
            const int maxGap = std::max(2, int(0.3 * Hf));   // a strand run ends at the first gap this tall (shadows and clothes further down are not hair)
            for (int y = std::max(0, int(fb.top())); y <= bottom && gapRows <= maxGap; ++y) {
                const uchar* sl = img.constScanLine(y);
                int leftN = 0, rightN = 0, leftEdge = int(fb.left()), rightEdge = int(fb.right());
                const bool belowChin = y > int(fb.bottom());
                for (int x = left; x <= right; ++x) {
                    if (!on(matte, x, y) || on(ovalMask, x, y) || !isHair(sl, x)) continue;
                    if (belowChin && std::fabs(x - fb.center().x()) < 0.55 * Wf) continue;   // the neck and its shadow
                    if (x < fb.center().x()) { ++leftN; leftEdge = std::min(leftEdge, x); }
                    else { ++rightN; rightEdge = std::max(rightEdge, x); }
                }
                const int need = std::max(2, int(0.04 * Wf));
                if (leftN >= need || rightN >= need) { lowest = y; gapRows = 0; } else ++gapRows;
                if (y >= int(eyeY - 0.15 * Hf) && y <= int(eyeY + 0.35 * Hf)) {
                    sideSum += ((fb.left() - leftEdge) + (rightEdge - fb.right())) / 2.0 / Wf;
                    ++sideRows;
                }
            }
            fa.hairSide = sideRows > 0 ? float(std::clamp(sideSum / sideRows, 0.0, 1.5)) : 0.0f;
            fa.hairLowest = float(std::clamp((lowest - fb.top()) / Hf, 0.0, 3.5));
            // fringe: hair-coloured pixels in the CENTRAL strip of the forehead (hair at its sides is a side sweep or a parting, not bangs)
            long fringeTotal = 0, fringeHair = 0;
            for (int y = std::max(0, int(fb.top() + 0.08 * Hf)); y <= std::min(H - 1, int(eyeY - 0.25 * Hf)); ++y) {
                const uchar* sl = img.constScanLine(y);
                for (int x = std::max(0, int(fb.center().x() - 0.22 * Wf)); x <= std::min(W - 1, int(fb.center().x() + 0.22 * Wf)); ++x) {
                    if (!on(ovalMask, x, y)) continue;
                    ++fringeTotal;
                    if (isHair(sl, x)) ++fringeHair;
                }
            }
            fa.hairFringe = fringeTotal > 0 ? float(fringeHair) / float(fringeTotal) : 0.0f;
            // strand direction coherence: the structure tensor of the luminance, per small block of hair (hair radiates from the crown, so
            // one tensor over the whole region would call straight hair incoherent), averaged weighted by the blocks' gradient energy
            const int blk = std::max(8, int(0.06 * Wf));
            auto lum = [](const uchar* r, int xx) { return 0.299 * r[xx * 3] + 0.587 * r[xx * 3 + 1] + 0.114 * r[xx * 3 + 2]; };
            double cohSum = 0.0, energySum = 0.0;
            for (int by = std::max(1, top); by + blk < std::min(H - 1, bottom); by += blk)
                for (int bx = std::max(1, left); bx + blk < std::min(W - 1, right); bx += blk) {
                    double sxx = 0, syy = 0, sxy = 0;
                    int inHair = 0, count = 0;
                    for (int y = by; y < by + blk; ++y) {
                        const uchar* a0 = img.constScanLine(y - 1);
                        const uchar* a1 = img.constScanLine(y);
                        const uchar* a2 = img.constScanLine(y + 1);
                        for (int x = bx; x < bx + blk; ++x) {
                            ++count;
                            if (!on(matte, x, y) || on(ovalMask, x, y) || !isHair(a1, x)) continue;
                            ++inHair;
                            const double gx = lum(a1, x + 1) - lum(a1, x - 1), gy = lum(a2, x) - lum(a0, x);
                            sxx += gx * gx; syy += gy * gy; sxy += gx * gy;
                        }
                    }
                    const double e = sxx + syy;
                    if (inHair < count * 3 / 4 || e < 400.0 * double(count)) continue;   // mostly hair, with real structure (not a flat patch)
                    cohSum += e * std::sqrt((sxx - syy) * (sxx - syy) + 4.0 * sxy * sxy) / e;
                    energySum += e;
                }
            fa.hairCoherence = energySum > 1e-6 ? float(cohSum / energySum) : 0.6f;   // no textured hair found: assume plain wavy-straight
            fa.hairShapeOk = true;
        }
        if (!fa.hair.ok) fa.note += QStringLiteral("no hair pixels found (bald, hat or occluded). ");
    }
    return fa;
}

// The matte in PHOTO coordinates (255 = person), rebuilt from the remover's cropped, re-padded RGBA output.
QImage personMatte(const QImage& photo)
{
    BackgroundRemover::Quality q = BackgroundRemover::Quality::Fast;
    const QString model = BackgroundRemover::resolveModelBlocking(BackgroundRemover::Quality::Best, &q);
    if (model.isEmpty()) return {};
    BackgroundRemover::Options ro;
    ro.keepAlpha = true;
    ro.quality = q;
    const auto cut = BackgroundRemover::removeBackground(photo, model, ro);
    if (qEnvironmentVariableIsSet("QTMESH_PHOTO_DEBUG"))
        fprintf(stderr, "photo-appearance: matte ok %d model %s err %s box %d,%d %dx%d\n", int(cut.ok), qPrintable(model), qPrintable(cut.error),
                cut.foregroundBox.x(), cut.foregroundBox.y(), cut.foregroundBox.width(), cut.foregroundBox.height());
    if (!cut.ok || cut.foregroundBox.isEmpty() || cut.image.isNull()) return {};
    const int outSz = cut.image.width(), fgW = cut.foregroundBox.width(), fgH = cut.foregroundBox.height();
    const int offX = (outSz - fgW) / 2, offY = (outSz - fgH) / 2;
    QImage m(photo.size(), QImage::Format_Grayscale8);
    m.fill(0);
    for (int y = 0; y < photo.height(); ++y)
        for (int x = 0; x < photo.width(); ++x) {
            const int ox = x - cut.foregroundBox.x() + offX, oy = y - cut.foregroundBox.y() + offY;
            if (ox >= 0 && oy >= 0 && ox < outSz && oy < outSz) m.scanLine(y)[x] = uchar(qAlpha(cut.image.pixel(ox, oy)));
        }
    return m;
}

QRectF headBoxFromBody(const std::vector<float>& v, const std::array<float, 3>& camT, float focal, int photoW, int photoH, float headHeight)
{
    if (v.size() < 9 || focal <= 0.0f) return {};
    float maxY = -1e9f;
    for (size_t i = 1; i < v.size(); i += 3) maxY = std::max(maxY, v[i]);
    double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        if (v[i + 1] < maxY - headHeight) continue;
        // SAM's camera space has y down and z away: the model's y and z are negated before the translation (checked on real photos).
        const double X = double(v[i]) + double(camT[0]), Y = -double(v[i + 1]) + double(camT[1]), Z = -double(v[i + 2]) + double(camT[2]);
        if (Z <= 1e-3) continue;
        const double u = double(focal) * X / Z + photoW * 0.5, w = double(focal) * Y / Z + photoH * 0.5;
        x0 = std::min(x0, u); x1 = std::max(x1, u); y0 = std::min(y0, w); y1 = std::max(y1, w);
    }
    if (x1 <= x0 || y1 <= y0) return {};
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

FaceAppearance estimate(const QImage& photo, const QRectF& personBoxIn, const QRectF& headBox, QImage* matteOut)
{
    FaceAppearance none;
    if (photo.isNull()) return none;
    const QImage matte = personMatte(photo);
    if (matteOut) *matteOut = matte;
    QRectF box = personBoxIn;
    if (!(box.width() > 0 && box.height() > 0)) box = QRectF(0, 0, photo.width(), photo.height());

    if (FaceRig::FaceLandmarkDetector::ensureModelBlocking().isEmpty()) { none.note = QStringLiteral("face landmark model unavailable"); return none; }
    FaceRig::FaceLandmarkDetector det;
    if (!det.load()) { none.note = QStringLiteral("face landmark model failed to load: ") + det.lastError(); return none; }

    // The head is unknown: try crops of several sizes anchored at the top of the person box and keep the most face-like one.
    // Centre column = where the matte is in the top strip of the box (the head), else the box centre.
    double cx = box.center().x();
    if (!matte.isNull()) {
        double sx = 0; int n = 0;
        const int yEnd = int(box.top() + box.height() * 0.08);
        for (int y = std::max(0, int(box.top())); y < std::min(photo.height(), yEnd + 1); ++y)
            for (int x = std::max(0, int(box.left())); x < std::min(photo.width(), int(box.right())); ++x)
                if (on(matte, x, y)) { sx += x; ++n; }
        if (n > 20) cx = sx / n;
    }
    FaceRig::LandmarkResult best;
    struct Crop { int ox, oy, side; };
    std::vector<Crop> crops;
    const int maxSide = std::min(photo.width(), photo.height());
    auto addCrop = [&](double cxp, double cyp, double side) {
        const int sd = int(std::clamp(side, 24.0, double(maxSide)));
        crops.push_back({std::clamp(int(cxp - sd / 2.0), 0, std::max(0, photo.width() - sd)), std::clamp(int(cyp - sd / 2.0), 0, std::max(0, photo.height() - sd)), sd});
    };
    if (headBox.isValid() && headBox.width() > 8) {
        // The head box (SAM's own head projected into the photo) covers head + neck; the face is in its upper two thirds.
        const double s = std::max(headBox.width(), headBox.height());
        const double fx = headBox.center().x();
        // the presence score of the landmark model depends a lot on the crop: try a few scales and heights and keep the best
        for (double dy : {0.35, 0.45, 0.55})
            for (double k : {1.0, 1.25, 1.6, 2.1}) addCrop(fx, headBox.top() + headBox.height() * dy, s * k);
    } else {
        for (double frac : {1.0 / 7.0, 1.0 / 5.5, 1.0 / 4.0, 1.0 / 2.8, 1.0 / 1.8})
            addCrop(cx, box.top() + box.height() * frac * 0.65, box.height() * frac * 1.5);
    }
    for (const Crop& c : crops) {
        const QImage crop = photo.copy(c.ox, c.oy, c.side, c.side);
        auto r = det.detect(crop);
        if (qEnvironmentVariableIsSet("QTMESH_PHOTO_DEBUG"))
            fprintf(stderr, "photo-appearance: crop %d %d %d ok %d logit %.2f matte %s\n", c.ox, c.oy, c.side, int(r.ok), double(r.presenceLogit), matte.isNull() ? "none" : "yes");
        if (r.ok && r.presenceLogit > best.presenceLogit) {
            for (auto& p : r.points) { p[0] += float(c.ox); p[1] += float(c.oy); }
            best = r;
        }
    }
    if (!best.ok || best.presenceLogit < 0.5f) { none.note = QStringLiteral("no face found in the photo"); return none; }
    return fromLandmarks(photo, best.points, matte);
}

namespace {
struct Pt2 { double x, y; };
// Photo-px projection of every vertex (SAM camera: y down, z away, see headBoxFromBody).
std::vector<Pt2> projectAll(const std::vector<float>& v, const std::array<float, 3>& camT, float focal, int W, int H)
{
    std::vector<Pt2> out(v.size() / 3, Pt2{-1e9, -1e9});
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        const double X = double(v[i]) + double(camT[0]), Y = -double(v[i + 1]) + double(camT[1]), Z = -double(v[i + 2]) + double(camT[2]);
        if (Z > 1e-3) out[i / 3] = {double(focal) * X / Z + W * 0.5, double(focal) * Y / Z + H * 0.5};
    }
    return out;
}

QImage rasterRegion(int W, int H, const std::vector<Pt2>& proj, const std::vector<int>& tris, const std::vector<char>& inSet)
{
    QImage m(W, H, QImage::Format_Grayscale8);
    m.fill(0);
    QPainter p(&m);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const int a = tris[t], b = tris[t + 1], c = tris[t + 2];
        const int n = int(std::min(proj.size(), inSet.size()));
        if (a < 0 || b < 0 || c < 0 || a >= n || b >= n || c >= n) continue;   // a corrupt index must not read past the arrays
        if (!inSet[size_t(a)] || !inSet[size_t(b)] || !inSet[size_t(c)]) continue;
        QPolygonF pf;
        pf << QPointF(proj[size_t(a)].x, proj[size_t(a)].y) << QPointF(proj[size_t(b)].x, proj[size_t(b)].y) << QPointF(proj[size_t(c)].x, proj[size_t(c)].y);
        p.drawPolygon(pf);
    }
    return m;
}

QImage intersect(const QImage& a, const QImage& b)
{
    QImage o = a;
    for (int y = 0; y < o.height(); ++y) {
        uchar* d = o.scanLine(y);
        for (int x = 0; x < o.width(); ++x) if (!on(b, x, y)) d[x] = 0;
    }
    return o;
}

// Photo pixels inside the mask that are not part of the person's skin-coloured exposed areas are NOT removed: the caller compares the median to the skin.
bool isSkinLike(const ColourEstimate& c, const std::array<float, 3>& skinLab, float tol)
{
    return !c.ok || dist(toLab(c.colour[0] * 255.0f, c.colour[1] * 255.0f, c.colour[2] * 255.0f), skinLab) < tol;
}
}  // namespace

ClothingEstimate estimateClothing(const QImage& photo, const QImage& matte, const Rgb& skin, const std::vector<float>& neutralVertices,
                                  const std::vector<float>& posedVertices, const std::vector<int>& triangles, const std::array<float, 3>& camT, float focal)
{
    ClothingEstimate ce;
    const size_t n = posedVertices.size() / 3;
    if (photo.isNull() || matte.isNull() || n == 0 || neutralVertices.size() != posedVertices.size() || triangles.size() < 3) { ce.note = QStringLiteral("no body mesh"); return ce; }
    const int W = photo.width(), H = photo.height();
    const std::vector<Pt2> proj = projectAll(posedVertices, camT, focal, W, H);
    float minY = 1e9f, maxY = -1e9f;
    for (size_t i = 0; i < n; ++i) { minY = std::min(minY, neutralVertices[i * 3 + 1]); maxY = std::max(maxY, neutralVertices[i * 3 + 1]); }
    const float height = std::max(1e-3f, maxY - minY);
    auto t = [&](size_t i) { return (neutralVertices[i * 3 + 1] - minY) / height; };
    auto ax = [&](size_t i) { return std::fabs(neutralVertices[i * 3]); };

    // Arms: neutral-pose vertices above the hips and away from the torso, split by distance from the body axis into upper arm / forearm.
    std::vector<float> armX;
    for (size_t i = 0; i < n; ++i) if (t(i) > 0.55f && ax(i) > 0.20f) armX.push_back(ax(i));
    std::sort(armX.begin(), armX.end());
    const float upTo = armX.empty() ? 1e9f : armX[size_t(double(armX.size()) * 0.42)], foreTo = armX.empty() ? 1e9f : armX[size_t(double(armX.size()) * 0.80)];

    std::vector<char> torso(n), upper(n), fore(n), thigh(n), shin(n);
    for (size_t i = 0; i < n; ++i) {
        const float ti = t(i), xi = ax(i);
        torso[i] = ti > 0.64f && ti < 0.76f && xi < 0.15f;   // the chest: a crop top covers it, the midriff often is bare
        upper[i] = ti > 0.55f && xi > 0.20f && xi <= upTo;
        fore[i] = ti > 0.55f && xi > upTo && xi <= foreTo;
        thigh[i] = ti > 0.28f && ti < 0.46f && xi < 0.25f;
        shin[i] = ti > 0.08f && ti < 0.26f && xi < 0.25f;
    }
    auto colourOf = [&](const std::vector<char>& set) {
        return robustColour(photo, intersect(rasterRegion(W, H, proj, triangles, set), matte), 16.0f, 80);
    };
    const ColourEstimate cTorso = colourOf(torso), cUpper = colourOf(upper), cFore = colourOf(fore), cThigh = colourOf(thigh), cShin = colourOf(shin);
    const auto skinLab = toLab(skin[0] * 255.0f, skin[1] * 255.0f, skin[2] * 255.0f);
    const float tol = 22.0f;
    if (cTorso.ok && !isSkinLike(cTorso, skinLab, tol)) {
        ce.top = !isSkinLike(cFore, skinLab, tol) && cFore.ok ? QStringLiteral("longsleeve")
                 : (cUpper.ok && !isSkinLike(cUpper, skinLab, tol)) ? QStringLiteral("tshirt") : QStringLiteral("tank");
        ce.topColour = cTorso;
    }
    if (cThigh.ok && !isSkinLike(cThigh, skinLab, tol)) {
        ce.bottom = (cShin.ok && !isSkinLike(cShin, skinLab, tol)) ? QStringLiteral("pants") : QStringLiteral("shorts");
        ce.bottomColour = cThigh;
    }
    ce.ok = true;
    return ce;
}

namespace {
struct FitModel {
    bool ok = false;
    std::vector<float> w;          // (modes + 1) x (points * 3): the mode rows, then the mean (intercept) row
    int modes = 0, points = 0;
    std::vector<int> order;        // modes by influence on the landmarks (largest first)
};
const FitModel& fitModel()
{
    static const FitModel m = [] {
        FitModel f;
        QFile file(QStringLiteral(":/character/face_landmark_fit.bin"));
        if (!file.open(QIODevice::ReadOnly)) return f;
        const QByteArray b = file.readAll();
        if (b.size() < 12 || b.left(4) != "MFL1") return f;
        int32_t modes = 0, pts = 0;
        memcpy(&modes, b.constData() + 4, 4);
        memcpy(&pts, b.constData() + 8, 4);
        const size_t n = size_t(modes + 1) * size_t(pts) * 3;
        if (modes <= 0 || pts <= 0 || size_t(b.size()) != 12 + n * 4) return f;
        f.w.resize(n);
        memcpy(f.w.data(), b.constData() + 12, n * 4);
        f.modes = modes; f.points = pts;
        std::vector<double> infl(size_t(modes), 0.0);
        for (int m2 = 0; m2 < modes; ++m2)
            for (int p = 0; p < pts; ++p)
                for (int k = 0; k < 2; ++k) { const double v = double(f.w[size_t(m2) * size_t(pts) * 3 + size_t(p) * 3 + size_t(k)]); infl[size_t(m2)] += v * v; }
        f.order.resize(size_t(modes));
        for (int i = 0; i < modes; ++i) f.order[size_t(i)] = i;
        std::sort(f.order.begin(), f.order.end(), [&](int a, int b2) { return infl[size_t(a)] > infl[size_t(b2)]; });
        f.ok = true;
        return f;
    }();
    return m;
}

// Landmarks centred and scaled to unit xy RMS radius.
std::vector<float> normalizedLandmarks(const std::vector<std::array<float, 3>>& lm, int points)
{
    std::vector<float> q(size_t(points) * 3);
    double c[3] = {0, 0, 0};
    for (int i = 0; i < points; ++i) for (int k = 0; k < 3; ++k) c[k] += double(lm[size_t(i)][size_t(k)]);
    for (double& v : c) v /= points;
    double r2 = 0;
    for (int i = 0; i < points; ++i) {
        for (int k = 0; k < 3; ++k) q[size_t(i) * 3 + size_t(k)] = float(double(lm[size_t(i)][size_t(k)]) - c[k]);
        r2 += double(q[size_t(i) * 3]) * double(q[size_t(i) * 3]) + double(q[size_t(i) * 3 + 1]) * double(q[size_t(i) * 3 + 1]);
    }
    const float s = float(std::sqrt(r2 / points));
    if (s > 1e-6f) for (float& v : q) v /= s;
    return q;
}

void modelLandmarks(const FitModel& m, const std::vector<float>& c, std::vector<float>& out)
{
    const size_t n = size_t(m.points) * 3;
    out.assign(m.w.begin() + long(size_t(m.modes) * n), m.w.begin() + long(size_t(m.modes + 1) * n));
    for (int i = 0; i < m.modes; ++i)
        for (size_t j = 0; j < n; ++j) out[j] += c[size_t(i)] * m.w[size_t(i) * n + j];
}

// photo landmarks (normalised) aligned onto the model's by a 3D similarity.
std::vector<float> alignedTo(const FitModel& m, const std::vector<float>& photoN, const std::vector<float>& model)
{
    std::vector<float> ones(size_t(m.points), 1.0f);
    const FaceCapPose::Result r = FaceCapPose::solve(photoN.data(), model.data(), ones.data(), m.points);
    std::vector<float> a(photoN.size());
    if (!r.ok) return photoN;
    const float x = r.rotation[0], y = r.rotation[1], z = r.rotation[2], w = r.rotation[3];
    const float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                        2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                        2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
    for (int i = 0; i < m.points; ++i)
        for (int k = 0; k < 3; ++k)
            a[size_t(i) * 3 + size_t(k)] = r.scale * (R[k * 3] * photoN[size_t(i) * 3] + R[k * 3 + 1] * photoN[size_t(i) * 3 + 1] + R[k * 3 + 2] * photoN[size_t(i) * 3 + 2]) + r.translation[size_t(k)];
    return a;
}

float landmarkError(const FitModel& m, const std::vector<float>& aligned, const std::vector<float>& model)
{
    // mean xy distance / inter-ocular distance (MediaPipe outer eye corners 33 and 263)
    const double iod = std::hypot(double(model[33 * 3] - model[263 * 3]), double(model[33 * 3 + 1] - model[263 * 3 + 1]));
    double e = 0;
    for (int i = 0; i < m.points; ++i) e += std::hypot(double(aligned[size_t(i) * 3] - model[size_t(i) * 3]), double(aligned[size_t(i) * 3 + 1] - model[size_t(i) * 3 + 1]));
    return float(e / m.points / std::max(iod, 1e-9));
}

// Solve the small SPD system A x = b (Gaussian elimination with partial pivoting).
bool solveDense(std::vector<double> A, std::vector<double> b, int n, std::vector<double>& x)
{
    for (int c = 0; c < n; ++c) {
        int p = c;
        for (int r = c + 1; r < n; ++r) if (std::fabs(A[size_t(r) * size_t(n) + size_t(c)]) > std::fabs(A[size_t(p) * size_t(n) + size_t(c)])) p = r;
        if (std::fabs(A[size_t(p) * size_t(n) + size_t(c)]) < 1e-12) return false;
        if (p != c) { for (int k = 0; k < n; ++k) std::swap(A[size_t(p) * size_t(n) + size_t(k)], A[size_t(c) * size_t(n) + size_t(k)]); std::swap(b[size_t(p)], b[size_t(c)]); }
        for (int r = c + 1; r < n; ++r) {
            const double f = A[size_t(r) * size_t(n) + size_t(c)] / A[size_t(c) * size_t(n) + size_t(c)];
            for (int k = c; k < n; ++k) A[size_t(r) * size_t(n) + size_t(k)] -= f * A[size_t(c) * size_t(n) + size_t(k)];
            b[size_t(r)] -= f * b[size_t(c)];
        }
    }
    x.assign(size_t(n), 0.0);
    for (int r = n - 1; r >= 0; --r) {
        double s = b[size_t(r)];
        for (int k = r + 1; k < n; ++k) s -= A[size_t(r) * size_t(n) + size_t(k)] * x[size_t(k)];
        x[size_t(r)] = s / A[size_t(r) * size_t(n) + size_t(r)];
    }
    return true;
}
}  // namespace

FaceFit fitFaceCoefficients(const std::vector<std::array<float, 3>>& photo, const std::vector<float>& sam, int modes, float lambda)
{
    FaceFit ff;
    const FitModel& m = fitModel();
    if (!m.ok) { ff.note = QStringLiteral("face-fit model resource missing"); return ff; }
    if (photo.size() < size_t(m.points) || sam.size() < size_t(m.modes)) { ff.note = QStringLiteral("no face landmarks"); return ff; }
    const int K = std::clamp(modes, 1, m.modes);
    const std::vector<float> pn = normalizedLandmarks(photo, m.points);
    const size_t n = size_t(m.points) * 3;
    std::vector<float> c(sam.begin(), sam.begin() + m.modes), model;
    std::vector<char> inSet(size_t(m.modes), 0);
    for (int i = 0; i < K; ++i) inSet[size_t(m.order[size_t(i)])] = 1;
    modelLandmarks(m, c, model);
    ff.errBefore = landmarkError(m, alignedTo(m, pn, model), model);
    for (int it = 0; it < 6; ++it) {
        modelLandmarks(m, c, model);
        const std::vector<float> a = alignedTo(m, pn, model);
        // target for the free modes: aligned photo - mean - contribution of the frozen modes
        std::vector<double> y(n);
        for (size_t j = 0; j < n; ++j) {
            double v = double(a[j]) - double(m.w[size_t(m.modes) * n + j]);
            for (int i = 0; i < m.modes; ++i) if (!inSet[size_t(i)]) v -= double(sam[size_t(i)]) * double(m.w[size_t(i) * n + j]);
            y[j] = v;
        }
        std::vector<double> A(size_t(K) * size_t(K), 0.0), b(size_t(K), 0.0);
        for (size_t j = 0; j < n; ++j) {
            const double wt = (j % 3 == 2) ? 0.25 : 1.0;   // the detector's depth is the least reliable axis
            for (int p = 0; p < K; ++p) {
                const double wp = double(m.w[size_t(m.order[size_t(p)]) * n + j]) * wt;
                b[size_t(p)] += wp * y[j];
                for (int q = 0; q < K; ++q) A[size_t(p) * size_t(K) + size_t(q)] += wp * double(m.w[size_t(m.order[size_t(q)]) * n + j]);
            }
        }
        for (int p = 0; p < K; ++p) { A[size_t(p) * size_t(K) + size_t(p)] += double(lambda); b[size_t(p)] += double(lambda) * double(sam[size_t(m.order[size_t(p)])]); }
        std::vector<double> x;
        if (!solveDense(A, b, K, x)) { ff.note = QStringLiteral("singular fit"); return ff; }
        c.assign(sam.begin(), sam.begin() + m.modes);
        for (int p = 0; p < K; ++p) c[size_t(m.order[size_t(p)])] = float(std::clamp(x[size_t(p)], -3.0, 3.0));
    }
    modelLandmarks(m, c, model);
    ff.errAfter = landmarkError(m, alignedTo(m, pn, model), model);
    ff.coefficients = sam;
    for (int i = 0; i < m.modes; ++i) ff.coefficients[size_t(i)] = c[size_t(i)];
    ff.modesUsed = K;
    ff.ok = true;
    return ff;
}


FaceProjection projectFace(const QImage& photo, const QImage& matte, const QRectF& headBox, const std::vector<float>& v,
                           const std::vector<int>& tris, const std::array<float, 3>& camT, float focal,
                           const std::vector<std::array<float, 3>>& landmarks)
{
    FaceProjection out;
    const size_t n = v.size() / 3;
    if (photo.isNull() || n == 0 || focal <= 0.0f || headBox.isEmpty()) return out;
    const float cx = float(photo.width()) * 0.5f, cy = float(photo.height()) * 0.5f;
    std::vector<std::array<float, 3>> cam(n);
    out.pixel.assign(n * 2, 0.0f);
    out.weight.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        cam[i] = {v[i * 3] + camT[0], -v[i * 3 + 1] + camT[1], -v[i * 3 + 2] + camT[2]};   // the SAM camera convention
        if (cam[i][2] <= 1e-3f) continue;
        out.pixel[i * 2] = focal * cam[i][0] / cam[i][2] + cx;
        out.pixel[i * 2 + 1] = focal * cam[i][1] / cam[i][2] + cy;
    }
    std::vector<std::array<float, 3>> nrm(n, std::array<float, 3>{0, 0, 0});
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const int a = tris[t], b = tris[t + 1], c = tris[t + 2];
        if (a < 0 || b < 0 || c < 0 || size_t(a) >= n || size_t(b) >= n || size_t(c) >= n) continue;
        const auto& A = cam[size_t(a)]; const auto& B = cam[size_t(b)]; const auto& C = cam[size_t(c)];
        const std::array<float, 3> e1{B[0] - A[0], B[1] - A[1], B[2] - A[2]}, e2{C[0] - A[0], C[1] - A[1], C[2] - A[2]};
        const std::array<float, 3> f{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (int idx : {a, b, c}) for (size_t k = 0; k < 3; ++k) nrm[size_t(idx)][k] += f[k];
    }
    std::vector<float> facing(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const float nl = std::sqrt(nrm[i][0] * nrm[i][0] + nrm[i][1] * nrm[i][1] + nrm[i][2] * nrm[i][2]);
        const float pl = std::sqrt(cam[i][0] * cam[i][0] + cam[i][1] * cam[i][1] + cam[i][2] * cam[i][2]);
        if (nl < 1e-12f || pl < 1e-6f) continue;
        facing[i] = -(nrm[i][0] * cam[i][0] + nrm[i][1] * cam[i][1] + nrm[i][2] * cam[i][2]) / (nl * pl);
    }
    // the winding may be either way round: the vertices nearest the camera must face it
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    const size_t nearest = std::max<size_t>(1, n / 20);
    std::partial_sort(order.begin(), order.begin() + long(nearest), order.end(), [&](size_t x, size_t y) { return cam[x][2] < cam[y][2]; });
    float mean = 0.0f;
    for (size_t k = 0; k < nearest; ++k) mean += facing[order[k]];
    const float sign = mean >= 0.0f ? 1.0f : -1.0f;
    const float feather = std::max(4.0f, float(std::min(headBox.width(), headBox.height())) * 0.12f);
    auto ramp = [](float a, float b, float t) { const float u = std::clamp((t - a) / (b - a), 0.0f, 1.0f); return u * u * (3.0f - 2.0f * u); };
    const bool haveMatte = !matte.isNull() && matte.size() == photo.size();
    // soft face-oval mask: the oval shrunk a little, blurred by a down/up scale so the projection fades out instead of ending in a line
    QImage oval;
    if (landmarks.size() >= 468) {
        oval = polygonMask(photo.width(), photo.height(), scaledPolygon(pts(landmarks, kOval), 0.95));
        const QSize small(std::max(8, photo.width() / 32), std::max(8, photo.height() / 32));
        oval = oval.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).scaled(photo.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    for (size_t i = 0; i < n; ++i) {
        const float x = out.pixel[i * 2], y = out.pixel[i * 2 + 1];
        if (cam[i][2] <= 1e-3f || x < 0 || y < 0 || x >= float(photo.width()) || y >= float(photo.height())) continue;
        const float edge = std::min({x - float(headBox.left()), float(headBox.right()) - x, y - float(headBox.top()), float(headBox.bottom()) - y});
        float w = ramp(0.0f, feather, edge) * ramp(0.25f, 0.6f, sign * facing[i]);
        if (haveMatte && qGray(matte.pixel(int(x), int(y))) < 128) w = 0.0f;
        if (!oval.isNull()) w *= ramp(0.15f, 0.85f, float(qGray(oval.pixel(int(x), int(y)))) / 255.0f);
        out.weight[i] = w;
        if (w > 0.5f) ++out.visible;
    }
    out.ok = out.visible > 50;
    return out;
}


HairChoice chooseHair(const FaceAppearance& fa)
{
    HairChoice c;
    if (!fa.hairShapeOk) return c;
    const float curl = std::clamp((0.55f - fa.hairCoherence) / 0.35f, 0.0f, 1.0f);
    if (fa.hairTop < 0.04f && fa.hairSide < 0.04f && fa.hairLowest < 0.45f) { c.preset = QStringLiteral("buzz"); return c; }
    const bool puffy = fa.hairTop > 0.38f && fa.hairSide > 0.28f && fa.hairLowest < 1.3f;   // a lot of hair standing off the head, not hanging
    if ((fa.hairTop > 0.55f && curl > 0.35f) || puffy) { c.preset = QStringLiteral("afro"); }   // (measured on real photos: curly hair is NOT incoherent block by block, so the shape decides)
    else if (fa.hairLowest > 1.45f) c.preset = curl > 0.4f ? QStringLiteral("long_wavy") : QStringLiteral("long_straight");
    else if (fa.hairLowest > 1.05f) c.preset = curl > 0.45f ? QStringLiteral("curly_bob") : (fa.hairLowest > 1.25f ? QStringLiteral("medium_layered") : QStringLiteral("bob"));
    else c.preset = fa.hairFringe > 0.3f ? QStringLiteral("short_side_part") : QStringLiteral("short_straight");
    const float lengthFace = std::max(0.0f, fa.hairLowest + fa.hairTop);   // from the top of the hair, in face heights
    c.length = std::clamp(lengthFace * 0.19f + 0.04f, 0.04f, 0.6f);
    c.volume = std::clamp(0.2f + 1.4f * fa.hairSide + 0.5f * std::max(0.0f, fa.hairTop - 0.3f), 0.1f, 1.0f);
    if (curl > 0.15f) c.curliness = curl;
    c.fringe = fa.hairFringe > 0.2f ? std::clamp((fa.hairFringe - 0.1f) * 1.6f, 0.0f, 1.0f) : 0.0f;   // always set: some presets carry their own bangs
    return c;
}

}  // namespace PhotoAppearance
}  // namespace CharacterGen
