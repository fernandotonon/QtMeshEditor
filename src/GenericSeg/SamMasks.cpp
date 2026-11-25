#include "SamMasks.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace GenericSeg::Sam {

std::vector<std::array<float, 2>> gridPoints(const Raster& r, int perSide)
{
    std::vector<std::array<float, 2>> out;
    if (r.size <= 0 || perSide <= 0) return out;
    const float toInput = float(kInputSize) / float(r.size);
    for (int j = 0; j < perSide; ++j)
        for (int i = 0; i < perSide; ++i) {
            const float fx = (float(i) + 0.5f) / float(perSide);
            const float fy = (float(j) + 0.5f) / float(perSide);
            const int px = std::min(r.size - 1, int(fx * float(r.size)));
            const int py = std::min(r.size - 1, int(fy * float(r.size)));
            if (r.at(px, py) < 0) continue;
            out.push_back({fx * float(r.size) * toInput, fy * float(r.size) * toInput});
        }
    return out;
}

float stabilityScore(const float* logits, int count, float offset)
{
    int inter = 0, uni = 0;
    for (int i = 0; i < count; ++i) {
        inter += logits[i] > offset;
        uni += logits[i] > -offset;
    }
    return uni > 0 ? float(inter) / float(uni) : 0.0f;
}

bool binarize(Candidate& c)
{
    const int n = kLowRes * kLowRes;
    c.low.assign(n, 0);
    c.area = 0;
    c.box = {kLowRes, kLowRes, -1, -1};
    for (int y = 0; y < kLowRes; ++y)
        for (int x = 0; x < kLowRes; ++x) {
            if (!(c.logits[y * kLowRes + x] > 0.0f)) continue;
            c.low[size_t(y) * kLowRes + x] = 1;
            ++c.area;
            c.box = {std::min(c.box[0], x), std::min(c.box[1], y),
                     std::max(c.box[2], x), std::max(c.box[3], y)};
        }
    return c.area > 0;
}

namespace {

float maskIou(const Candidate& a, const Candidate& b)
{
    const int x0 = std::max(a.box[0], b.box[0]), y0 = std::max(a.box[1], b.box[1]);
    const int x1 = std::min(a.box[2], b.box[2]), y1 = std::min(a.box[3], b.box[3]);
    if (x0 > x1 || y0 > y1) return 0.0f;
    int inter = 0;
    for (int y = y0; y <= y1; ++y) {
        const uint8_t* pa = a.low.data() + size_t(y) * kLowRes;
        const uint8_t* pb = b.low.data() + size_t(y) * kLowRes;
        for (int x = x0; x <= x1; ++x) inter += pa[x] & pb[x];
    }
    const int uni = a.area + b.area - inter;
    return uni > 0 ? float(inter) / float(uni) : 0.0f;
}

} // namespace

std::vector<int> nms(const std::vector<Candidate>& candidates, float iouThreshold)
{
    std::vector<int> order(candidates.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return candidates[a].score > candidates[b].score;
    });
    std::vector<int> kept;
    for (int i : order) {
        bool keep = true;
        for (int k : kept)
            if (maskIou(candidates[i], candidates[k]) > iouThreshold) { keep = false; break; }
        if (keep) kept.push_back(i);
    }
    return kept;
}

std::vector<uint8_t> upsample(const float* logits, int size)
{
    std::vector<uint8_t> out(size_t(size) * size, 0);
    const float scale = float(kLowRes) / float(size);
    for (int y = 0; y < size; ++y) {
        const float sy = std::clamp((float(y) + 0.5f) * scale - 0.5f, 0.0f, float(kLowRes - 1));
        const int y0 = int(sy), y1 = std::min(y0 + 1, kLowRes - 1);
        const float wy = sy - float(y0);
        for (int x = 0; x < size; ++x) {
            const float sx = std::clamp((float(x) + 0.5f) * scale - 0.5f, 0.0f, float(kLowRes - 1));
            const int x0 = int(sx), x1 = std::min(x0 + 1, kLowRes - 1);
            const float wx = sx - float(x0);
            const float top = logits[y0 * kLowRes + x0] * (1 - wx) + logits[y0 * kLowRes + x1] * wx;
            const float bot = logits[y1 * kLowRes + x0] * (1 - wx) + logits[y1 * kLowRes + x1] * wx;
            out[size_t(y) * size + x] = (top * (1 - wy) + bot * wy) > 0.0f;
        }
    }
    return out;
}

} // namespace GenericSeg::Sam
