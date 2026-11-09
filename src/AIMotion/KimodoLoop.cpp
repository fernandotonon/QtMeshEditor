#include "KimodoLoop.h"

#include "KimodoMath.h"

#include <algorithm>
#include <cmath>

namespace AIMotion::KimodoLoop {

namespace {

constexpr double kRadToDeg = 57.29577951308232;

// Angle between two rotation matrices: acos((trace(A^T B) - 1) / 2).
double angleDeg(const float* a, const float* b)
{
    double tr = 0.0;
    for (int k = 0; k < 9; ++k) tr += double(a[k]) * double(b[k]);
    return std::acos(std::clamp((tr - 1.0) * 0.5, -1.0, 1.0)) * kRadToDeg;
}

std::array<float, 4> slerp(std::array<float, 4> a, std::array<float, 4> b, float t)
{
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0f) {
        for (float& v : b) v = -v;
        dot = -dot;
    }
    std::array<float, 4> out{};
    if (dot > 0.9995f) {
        for (int k = 0; k < 4; ++k) out[size_t(k)] = a[size_t(k)] + t * (b[size_t(k)] - a[size_t(k)]);
    } else {
        const float th = std::acos(dot);
        const float s = std::sin(th);
        const float wa = std::sin((1.0f - t) * th) / s;
        const float wb = std::sin(t * th) / s;
        for (int k = 0; k < 4; ++k) out[size_t(k)] = wa * a[size_t(k)] + wb * b[size_t(k)];
    }
    const float n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    for (float& v : out) v /= n;
    return out;
}

Motion sliceFrames(const Motion& m, int first, int count)
{
    Motion out;
    out.T = count;
    out.J = m.J;
    out.C = m.C;
    const auto copy = [first, count](const std::vector<float>& src, size_t per, std::vector<float>* dst) {
        dst->assign(src.begin() + std::ptrdiff_t(size_t(first) * per), src.begin() + std::ptrdiff_t(size_t(first + count) * per));
    };
    copy(m.local, size_t(m.J) * 9, &out.local);
    copy(m.root, 3, &out.root);
    copy(m.contacts, size_t(m.C), &out.contacts);
    return out;
}

// Frame `dst` := frame `src` of the same motion, root shifted horizontally by `shift`.
void copyFrame(Motion* m, int src, int dst, const std::array<float, 2>& shift)
{
    const size_t perRot = size_t(m->J) * 9;
    std::copy_n(m->local.begin() + std::ptrdiff_t(size_t(src) * perRot), perRot, m->local.begin() + std::ptrdiff_t(size_t(dst) * perRot));
    std::copy_n(m->contacts.begin() + std::ptrdiff_t(size_t(src) * size_t(m->C)), size_t(m->C),
                m->contacts.begin() + std::ptrdiff_t(size_t(dst) * size_t(m->C)));
    m->root[size_t(dst) * 3] = m->root[size_t(src) * 3] + shift[0];
    m->root[size_t(dst) * 3 + 1] = m->root[size_t(src) * 3 + 1];
    m->root[size_t(dst) * 3 + 2] = m->root[size_t(src) * 3 + 2] + shift[1];
}

void rebaseRoot(Motion* m)
{
    if (m->T == 0) return;
    const float x0 = m->root[0];
    const float z0 = m->root[2];
    for (int t = 0; t < m->T; ++t) {
        m->root[size_t(t) * 3] -= x0;
        m->root[size_t(t) * 3 + 2] -= z0;
    }
}

} // namespace

QStringList modeIds() { return {QStringLiteral("auto"), QStringLiteral("cut"), QStringLiteral("regenerate")}; }

bool modeFromId(const QString& id, Mode* mode)
{
    const QString s = id.trimmed().toLower();
    if (s.isEmpty() || s == QLatin1String("auto")) *mode = Mode::Auto;
    else if (s == QLatin1String("cut")) *mode = Mode::Cut;
    else if (s == QLatin1String("regenerate")) *mode = Mode::Regenerate;
    else return false;
    return true;
}

QString modeId(Mode mode)
{
    switch (mode) {
    case Mode::Cut: return QStringLiteral("cut");
    case Mode::Regenerate: return QStringLiteral("regenerate");
    case Mode::Auto: break;
    }
    return QStringLiteral("auto");
}

double frameCostDeg(const Motion& m, int a, int b)
{
    if (a < 0 || b < 0 || a >= m.T || b >= m.T) return std::numeric_limits<double>::infinity();
    double sum = 0.0;
    for (int jt = 0; jt < m.J; ++jt)
        sum += angleDeg(&m.local[(size_t(a) * size_t(m.J) + size_t(jt)) * 9], &m.local[(size_t(b) * size_t(m.J) + size_t(jt)) * 9]);
    // Root height in cm counts like degrees (a crouch that does not come back up is not a loop).
    sum += std::abs(double(m.root[size_t(a) * 3 + 1]) - double(m.root[size_t(b) * 3 + 1])) * 100.0;
    return sum / double(m.J + 1);
}

double windowCostDeg(const Motion& m, int i, int j, int window)
{
    double sum = 0.0;
    for (int k = 0; k < window; ++k) sum += frameCostDeg(m, i + k, j + k);
    return window > 0 ? sum / window : std::numeric_limits<double>::infinity();
}

Cycle findCycle(const Motion& m, const SearchOptions& o)
{
    Cycle best;
    const int T = m.T;
    const int w = std::max(1, o.window);
    const int s0 = std::max(0, o.minStart);
    if (T <= s0 + w) return best;
    // Pairwise frame distances once: every candidate reads windows of them.
    std::vector<double> dist(size_t(T) * size_t(T), 0.0);
    for (int a = s0; a < T; ++a)
        for (int b = a + 1; b < T; ++b) dist[size_t(a) * T + size_t(b)] = dist[size_t(b) * T + size_t(a)] = frameCostDeg(m, a, b);
    const auto d = [&dist, T](int a, int b) { return dist[size_t(a) * T + size_t(b)]; };
    double reference = 0.0;   // how far the motion ever gets from its own start, after the lead-in
    for (int t = s0; t < T; ++t) reference = std::max(reference, d(s0, t));
    double bestScore = std::numeric_limits<double>::infinity();
    const int maxP = o.maxPeriod > 0 ? o.maxPeriod : T;
    for (int p = std::max(2, o.minPeriod); p <= maxP; ++p)
        for (int s = s0; s + p + w <= T; ++s) {
            double c = 0.0;
            for (int k = 0; k < w; ++k) c += d(s + k, s + p + k);
            c /= w;
            double score = c;
            if (o.targetPeriod > 0) score += o.lengthWeightDeg * std::abs(double(p - o.targetPeriod)) / double(o.targetPeriod);
            if (score >= bestScore) continue;
            double activity = 0.0;
            for (int t = 1; t < p; ++t) activity = std::max(activity, d(s, s + t));
            if (activity < o.minActivity * reference) continue;
            bestScore = score;
            best = {true, s, p, c};
        }
    return best;
}

std::array<float, 2> strideOf(const Motion& m, int from, int to)
{
    return {m.root[size_t(to) * 3] - m.root[size_t(from) * 3], m.root[size_t(to) * 3 + 2] - m.root[size_t(from) * 3 + 2]};
}

Motion cutLoop(const Motion& m, int start, int period, int blend)
{
    const std::array<float, 2> d = strideOf(m, start, start + period);
    Motion out = sliceFrames(m, start, period + 1);
    blend = std::clamp(blend, 0, std::min(start, period / 2));
    // Frame start+period-k fades toward frame start-k (shifted one stride), fully at the wrap.
    for (int k = 1; k <= blend; ++k) {
        const float w = float(blend - k + 1) / float(blend + 1);
        const int t = period - k;           // index in `out`
        const int src = start - k;          // index in `m`
        for (int j = 0; j < m.J; ++j) {
            float* dst = &out.local[(size_t(t) * size_t(m.J) + size_t(j)) * 9];
            const auto qa = KimodoMath::quatFromMatrixWxyz(dst);
            const auto qb = KimodoMath::quatFromMatrixWxyz(&m.local[(size_t(src) * size_t(m.J) + size_t(j)) * 9]);
            KimodoMath::matrixFromQuatWxyz(slerp(qa, qb, w), dst);
        }
        out.root[size_t(t) * 3] += w * (m.root[size_t(src) * 3] + d[0] - out.root[size_t(t) * 3]);
        out.root[size_t(t) * 3 + 1] += w * (m.root[size_t(src) * 3 + 1] - out.root[size_t(t) * 3 + 1]);
        out.root[size_t(t) * 3 + 2] += w * (m.root[size_t(src) * 3 + 2] + d[1] - out.root[size_t(t) * 3 + 2]);
        if (w >= 0.5f)
            std::copy_n(m.contacts.begin() + std::ptrdiff_t(size_t(src) * size_t(m.C)), size_t(m.C),
                        out.contacts.begin() + std::ptrdiff_t(size_t(t) * size_t(m.C)));
    }
    copyFrame(&out, 0, period, d);
    rebaseRoot(&out);
    return out;
}

Motion closeLoop(const Motion& m, int period, const std::array<float, 2>& stride)
{
    Motion out = sliceFrames(m, 0, period + 1);
    copyFrame(&out, 0, period, stride);
    rebaseRoot(&out);
    return out;
}

} // namespace AIMotion::KimodoLoop
