#include "FootContact.h"

#include <algorithm>
#include <cmath>

namespace FootContact {

namespace {

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 mul(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const V3& a, const V3& b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
V3 cross(const V3& a, const V3& b)
{
    return {a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]};
}
float len(const V3& a) { return std::sqrt(dot(a, a)); }
V3 normed(const V3& a)
{
    const float l = len(a);
    return l > 1e-9f ? mul(a, 1.0f / l) : V3{0, 0, 0};
}

}  // namespace

float groundLevel(const std::vector<V3>& foot, int upAxis)
{
    if (foot.empty()) return 0.0f;
    std::vector<float> h(foot.size());
    for (size_t i = 0; i < foot.size(); ++i) h[i] = foot[i][upAxis];
    const size_t k = std::min(foot.size() - 1,
                              static_cast<size_t>(foot.size() / 10));
    std::nth_element(h.begin(), h.begin() + static_cast<long>(k), h.end());
    return h[k];
}

std::vector<Span> detectContacts(const std::vector<V3>& foot,
                                 float legLength,
                                 const DetectOptions& opt)
{
    std::vector<Span> spans;
    const int n = static_cast<int>(foot.size());
    if (n < opt.minFrames || legLength <= 1e-6f) return spans;
    const int up = std::clamp(opt.upAxis, 0, 2);
    const float ground = groundLevel(foot, up);
    const float band = ground + opt.heightBandFrac * legLength;
    const float vmax = opt.velThreshFrac * legLength;

    auto horizSpeed = [&](int f) {
        // central difference where possible; edges take the one-sided step
        const int a = std::max(0, f - 1);
        const int b = std::min(n - 1, f + 1);
        V3 d = sub(foot[b], foot[a]);
        d[up] = 0.0f;
        return len(d) / static_cast<float>(std::max(1, b - a));
    };

    int start = -1;
    for (int f = 0; f <= n; ++f) {
        const bool contact = f < n && foot[f][up] <= band && horizSpeed(f) <= vmax;
        if (contact && start < 0) start = f;
        if (!contact && start >= 0) {
            if (f - start >= opt.minFrames) spans.push_back({start, f - 1});
            start = -1;
        }
    }
    return spans;
}

V3 solveKnee(const V3& hip, const V3& knee, const V3& foot, const V3& target)
{
    const float l1 = len(sub(knee, hip));
    const float l2 = len(sub(foot, knee));
    if (l1 <= 1e-6f || l2 <= 1e-6f) return knee;

    V3 toT = sub(target, hip);
    float d = len(toT);
    const float dMin = std::abs(l1 - l2) + 1e-4f;
    const float dMax = l1 + l2 - 1e-4f;
    if (d < 1e-6f) return knee;                    // target on the hip: give up
    const V3 dirT = mul(toT, 1.0f / d);
    d = std::clamp(d, dMin, dMax);

    // Bend direction: the current knee's offset from the hip→foot line keeps
    // the pose's own bend plane (the knee axis of the existing pose).
    V3 bend = sub(knee, hip);
    bend = sub(bend, mul(dirT, dot(bend, dirT)));
    if (dot(bend, bend) < 1e-10f) {
        // straight leg — bend forward of the current chain plane, or any
        // perpendicular when even that is degenerate
        bend = cross(dirT, cross(sub(foot, hip), sub(knee, hip)));
        if (dot(bend, bend) < 1e-10f)
            bend = cross(dirT, std::abs(dirT[1]) < 0.9f ? V3{0, 1, 0}
                                                        : V3{1, 0, 0});
    }
    bend = normed(bend);

    // law of cosines: knee at distance l1 from hip, l2 from target
    const float cosA = std::clamp((l1 * l1 + d * d - l2 * l2)
                                  / (2.0f * l1 * d), -1.0f, 1.0f);
    const float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
    return add(hip, add(mul(dirT, l1 * cosA), mul(bend, l1 * sinA)));
}

float blendWeight(const Span& s, int f, int blend)
{
    if (f < s.start || f > s.end) return 0.0f;
    if (blend <= 0) return 1.0f;
    const float in = static_cast<float>(f - s.start + 1) / blend;
    const float out = static_cast<float>(s.end - f + 1) / blend;
    return std::clamp(std::min(in, out), 0.0f, 1.0f);
}

}  // namespace FootContact
