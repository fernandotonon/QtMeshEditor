#include "FaceCapGeom.h"

#include <algorithm>
#include <cmath>

namespace FaceCapGeom {

namespace {

constexpr float kMinScale = 0.1484375f;
constexpr float kMaxScale = 0.75f;
constexpr float kAnchorOffset = 0.5f;

float sigmoid(float x)
{
    x = std::clamp(x, -100.f, 100.f);
    return x >= 0.f ? 1.f / (1.f + std::exp(-x))
                    : std::exp(x) / (1.f + std::exp(x));
}

float normalizeAngle(float a)
{
    return a - 2.f * static_cast<float>(M_PI)
                   * std::floor((a + static_cast<float>(M_PI))
                                / (2.f * static_cast<float>(M_PI)));
}

// Bilinear sample of an RGB888 image with zero border (MediaPipe BORDER_ZERO,
// non-antialiased — the parity requirement from the spike).
inline void sampleBilinear(const uchar* bits, int w, int h, qsizetype stride,
                           float x, float y, float rgb[3])
{
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - x0;
    const float fy = y - y0;
    float acc[3] = {0.f, 0.f, 0.f};
    for (int dy = 0; dy < 2; ++dy) {
        const int yy = y0 + dy;
        if (yy < 0 || yy >= h)
            continue;
        const float wy = dy ? fy : 1.f - fy;
        const uchar* row = bits + yy * stride;
        for (int dx = 0; dx < 2; ++dx) {
            const int xx = x0 + dx;
            if (xx < 0 || xx >= w)
                continue;
            const float wgt = wy * (dx ? fx : 1.f - fx);
            const uchar* px = row + xx * 3;
            acc[0] += wgt * px[0];
            acc[1] += wgt * px[1];
            acc[2] += wgt * px[2];
        }
    }
    rgb[0] = acc[0];
    rgb[1] = acc[1];
    rgb[2] = acc[2];
}

// Generic affine resample: out(u,v) <- img(origin + u*du + v*dv), the shared
// core of letterbox and rotated-crop. Indices follow cv2.warpAffine's
// integer-index convention — the one the spike's parity proof pinned down
// (docs/MOCAP_SPIKE.md); do NOT switch to pixel-centre sampling.
void resampleToTensor(const QImage& rgb888, int size, float lo, float hi,
                      float originX, float originY, float dux, float duy,
                      float dvx, float dvy, float* out)
{
    const uchar* bits = rgb888.constBits();
    const int w = rgb888.width();
    const int h = rgb888.height();
    const qsizetype stride = rgb888.bytesPerLine();
    const float scale = (hi - lo) / 255.f;
    float* dst = out;
    for (int v = 0; v < size; ++v) {
        for (int u = 0; u < size; ++u) {
            const float sx = originX + u * dux + v * dvx;
            const float sy = originY + u * duy + v * dvy;
            float rgb[3];
            sampleBilinear(bits, w, h, stride, sx, sy, rgb);
            *dst++ = rgb[0] * scale + lo;
            *dst++ = rgb[1] * scale + lo;
            *dst++ = rgb[2] * scale + lo;
        }
    }
}

float iou(const std::array<float, 4>& a, const std::array<float, 4>& b)
{
    const float x1 = std::max(a[0], b[0]);
    const float y1 = std::max(a[1], b[1]);
    const float x2 = std::min(a[0] + a[2], b[0] + b[2]);
    const float y2 = std::min(a[1] + a[3], b[1] + b[3]);
    const float inter = std::max(0.f, x2 - x1) * std::max(0.f, y2 - y1);
    const float uni = a[2] * a[3] + b[2] * b[3] - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

}  // namespace

Letterbox letterboxToTensor(const QImage& rgb888, int size, float lo, float hi,
                            float* out)
{
    Letterbox lb;
    const int w = rgb888.width();
    const int h = rgb888.height();
    if (w <= 0 || h <= 0)
        return lb;
    const float s = static_cast<float>(size) / std::max(w, h);
    const float ox = (size - w * s) / 2.f;
    const float oy = (size - h * s) / 2.f;
    lb.padX = ox / size;
    lb.padY = oy / size;
    lb.fracX = w * s / size;
    lb.fracY = h * s / size;
    // output (u,v) -> input ((u - ox) / s, (v - oy) / s)
    resampleToTensor(rgb888, size, lo, hi, -ox / s, -oy / s, 1.f / s, 0.f, 0.f,
                     1.f / s, out);
    return lb;
}

std::vector<std::array<float, 2>> genSsdAnchors(int inputSize,
                                                const std::vector<int>& strides)
{
    std::vector<std::array<float, 2>> anchors;
    const int n = static_cast<int>(strides.size());
    auto scaleFor = [&](int i) {
        return n == 1 ? (kMinScale + kMaxScale) * 0.5f
                      : kMinScale + (kMaxScale - kMinScale) * i / (n - 1);
    };
    int layer = 0;
    while (layer < n) {
        int last = layer;
        int scalesInLayer = 0;
        while (last < n && strides[last] == strides[layer]) {
            scalesInLayer += 2;  // scale + interpolated scale (aspect 1.0)
            (void)scaleFor(last);
            ++last;
        }
        const int stride = strides[layer];
        const int fm = (inputSize + stride - 1) / stride;
        for (int y = 0; y < fm; ++y)
            for (int x = 0; x < fm; ++x)
                for (int k = 0; k < scalesInLayer; ++k)
                    anchors.push_back({(x + kAnchorOffset) / fm,
                                       (y + kAnchorOffset) / fm});
        layer = last;
    }
    return anchors;
}

std::vector<Detection> decodeDetections(
    const float* rawBoxes, const float* rawScores,
    const std::vector<std::array<float, 2>>& anchors, int inputSize,
    int numKeypoints, float minScore, float iouThreshold)
{
    const int boxDim = 4 + 2 * numKeypoints;
    std::vector<Detection> candidates;
    for (size_t i = 0; i < anchors.size(); ++i) {
        const float score = sigmoid(rawScores[i]);
        if (score < minScore)
            continue;
        const float* box = rawBoxes + i * boxDim;
        Detection d;
        d.score = score;
        const float cx = box[0] / inputSize + anchors[i][0];
        const float cy = box[1] / inputSize + anchors[i][1];
        const float w = box[2] / inputSize;
        const float h = box[3] / inputSize;
        d.box = {cx - w / 2.f, cy - h / 2.f, w, h};
        d.keypoints.reserve(numKeypoints);
        for (int k = 0; k < numKeypoints; ++k)
            d.keypoints.push_back({box[4 + 2 * k] / inputSize + anchors[i][0],
                                   box[5 + 2 * k] / inputSize + anchors[i][1]});
        candidates.push_back(std::move(d));
    }

    // weighted NMS (MediaPipe): clusters above the IoU threshold merge into a
    // score-weighted average instead of being discarded.
    std::sort(candidates.begin(), candidates.end(),
              [](const Detection& a, const Detection& b) { return a.score > b.score; });
    std::vector<Detection> out;
    std::vector<bool> used(candidates.size(), false);
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (used[i])
            continue;
        std::vector<size_t> cluster{i};
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (!used[j]
                && iou(candidates[i].box, candidates[j].box) > iouThreshold)
                cluster.push_back(j);
        }
        float total = 0.f;
        Detection merged = candidates[i];
        std::fill(merged.box.begin(), merged.box.end(), 0.f);
        for (auto& kp : merged.keypoints)
            kp = {0.f, 0.f};
        for (size_t j : cluster) {
            used[j] = true;
            total += candidates[j].score;
        }
        for (size_t j : cluster) {
            const float w = candidates[j].score / total;
            for (int k = 0; k < 4; ++k)
                merged.box[k] += w * candidates[j].box[k];
            for (size_t k = 0; k < merged.keypoints.size(); ++k) {
                merged.keypoints[k][0] += w * candidates[j].keypoints[k][0];
                merged.keypoints[k][1] += w * candidates[j].keypoints[k][1];
            }
        }
        out.push_back(std::move(merged));
    }
    return out;
}

RotatedRect rectFromFaceDetection(const Detection& det, int imgW, int imgH)
{
    RotatedRect r;
    r.cx = (det.box[0] + det.box[2] / 2.f) * imgW;
    r.cy = (det.box[1] + det.box[3] / 2.f) * imgH;
    const float dx = (det.keypoints[1][0] - det.keypoints[0][0]) * imgW;
    const float dy = (det.keypoints[1][1] - det.keypoints[0][1]) * imgH;
    r.angle = normalizeAngle(-std::atan2(-dy, dx));  // target angle 0
    const float side = std::max(det.box[2] * imgW, det.box[3] * imgH) * 1.5f;
    r.w = r.h = side;
    return r;
}

RotatedRect rectFromFaceLandmarks(const float* landmarksXyz, int count,
                                  int imgW, int imgH)
{
    RotatedRect r;
    if (count < 264)
        return r;
    float minX = landmarksXyz[0], maxX = landmarksXyz[0];
    float minY = landmarksXyz[1], maxY = landmarksXyz[1];
    for (int i = 1; i < count; ++i) {
        minX = std::min(minX, landmarksXyz[i * 3]);
        maxX = std::max(maxX, landmarksXyz[i * 3]);
        minY = std::min(minY, landmarksXyz[i * 3 + 1]);
        maxY = std::max(maxY, landmarksXyz[i * 3 + 1]);
    }
    r.cx = (minX + maxX) / 2.f;
    r.cy = (minY + maxY) / 2.f;
    // eye outer corners: 33 (right), 263 (left) — MediaPipe tracking mode
    const float dx = landmarksXyz[263 * 3] - landmarksXyz[33 * 3];
    const float dy = landmarksXyz[263 * 3 + 1] - landmarksXyz[33 * 3 + 1];
    r.angle = normalizeAngle(-std::atan2(-dy, dx));
    const float side = std::max(maxX - minX, maxY - minY) * 1.5f;
    r.w = r.h = side;
    (void)imgW;
    (void)imgH;
    return r;
}

RotatedRect rectFromPoseDetection(const Detection& det, int imgW, int imgH)
{
    RotatedRect r;
    r.cx = det.keypoints[0][0] * imgW;
    r.cy = det.keypoints[0][1] * imgH;
    const float dx = det.keypoints[1][0] * imgW - r.cx;
    const float dy = det.keypoints[1][1] * imgH - r.cy;
    const float radius = std::sqrt(dx * dx + dy * dy);
    r.angle = normalizeAngle(static_cast<float>(M_PI) / 2.f - std::atan2(-dy, dx));
    r.w = r.h = 2.f * radius * 1.25f;
    return r;
}

void cropRotatedRectToTensor(const QImage& rgb888, const RotatedRect& rect,
                             int size, float lo, float hi, float* out)
{
    const float ca = std::cos(rect.angle);
    const float sa = std::sin(rect.angle);
    const float sx = rect.w / size;
    const float sy = rect.h / size;
    // p_img = centre + R(angle) . ((u/size - 0.5) * w, (v/size - 0.5) * h)
    const float originX = rect.cx + ca * (-rect.w / 2.f) - sa * (-rect.h / 2.f);
    const float originY = rect.cy + sa * (-rect.w / 2.f) + ca * (-rect.h / 2.f);
    resampleToTensor(rgb888, size, lo, hi, originX, originY, ca * sx, sa * sx,
                     -sa * sy, ca * sy, out);
}

void projectLandmarks(float* pts, int count, int stride, const RotatedRect& rect)
{
    const float ca = std::cos(rect.angle);
    const float sa = std::sin(rect.angle);
    for (int i = 0; i < count; ++i) {
        float* p = pts + i * stride;
        const float x = p[0] - 0.5f;
        const float y = p[1] - 0.5f;
        p[0] = rect.cx + x * rect.w * ca - y * rect.h * sa;
        p[1] = rect.cy + x * rect.w * sa + y * rect.h * ca;
        if (stride > 2)
            p[2] *= rect.w;
    }
}

}  // namespace FaceCapGeom
