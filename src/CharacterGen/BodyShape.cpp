#include "BodyShape.h"

#include <algorithm>
#include <cmath>

namespace CharacterGen {
namespace BodyShape {

namespace {
enum Class : uint8_t { Torso = 0, Leg, Arm, Collar, Neck, Head };

const Info kInfo[MeasureCount] = {
    {"height", "Height", "Overall height: the legs, torso and head grow together.", 12.0f},
    {"leg_length", "Leg length", "Floor to crotch. Positive = longer legs.", 8.0f},
    {"arm_span", "Arm length", "Hand to hand with the arms out. Positive = longer arms.", 10.0f},
    {"shoulder_width", "Shoulder width", "Distance between the shoulders.", 5.0f},
    {"chest_width", "Chest width", "Width of the ribcage.", 5.0f},
    {"waist_width", "Waist width", "Width at the waist: negative = a narrow waist.", 5.0f},
    {"hips_width", "Hips width", "Width at the hips: negative = narrow, positive = wide.", 5.0f},
    {"chest_depth", "Chest depth", "Front to back at the bust: a deeper (fuller) chest or a flat one.", 4.0f},
    {"belly_depth", "Belly depth", "Front to back at the waist: positive = a rounder belly.", 5.0f},
    {"neck_length", "Neck length", "Length of the neck.", 2.5f},
    {"head_height", "Head height", "Chin to top of the head.", 2.5f},
    {"head_width", "Head width", "Width of the head.", 2.0f},
};

Class classify(const QString& n)
{
    const QString l = n.toLower();
    if (l.contains(QLatin1String("clavicle"))) return Collar;
    for (const char* k : {"uparm", "lowarm", "wrist", "thumb", "index", "middle", "ring", "pinky"})
        if (l.contains(QLatin1String(k))) return Arm;
    for (const char* k : {"upleg", "lowleg", "foot", "ball", "talocrural", "subtalar", "transversetarsal"})
        if (l.contains(QLatin1String(k))) return Leg;
    if (l.contains(QLatin1String("neck"))) return Neck;
    for (const char* k : {"head", "jaw", "eye", "tongue", "teeth"})
        if (l.contains(QLatin1String(k))) return Head;
    return Torso;
}
}  // namespace

const Info& info(Measure m) { return kInfo[std::clamp(int(m), 0, int(MeasureCount) - 1)]; }

std::array<float, MeasureCount> Model::measureShaped(const std::vector<float>& P) const
{
    std::array<float, MeasureCount> out{};
    float minY = 1e9f, maxY = -1e9f;
    for (int v = 0; v < m_vertices; ++v) { minY = std::min(minY, P[size_t(v) * 3 + 1]); maxY = std::max(maxY, P[size_t(v) * 3 + 1]); }
    const float H = maxY - minY;
    out[Height] = H;
    auto extent = [&](int axis, auto&& pick, float y0, float y1) {
        float lo = 1e9f, hi = -1e9f;
        bool any = false;
        for (int v = 0; v < m_vertices; ++v) {
            const float y = P[size_t(v) * 3 + 1];
            if (!pick(m_class[size_t(v)]) || y < minY + y0 * H || y > minY + y1 * H) continue;
            const float c = P[size_t(v) * 3 + size_t(axis)];
            lo = std::min(lo, c); hi = std::max(hi, c); any = true;
        }
        return any ? hi - lo : 0.0f;
    };
    auto torsoOnly = [](uint8_t c) { return c == Torso; };
    auto trunk = [](uint8_t c) { return c == Torso || c == Leg; };
    // crotch: the lowest point of the pelvis-driven vertices
    float crotch = 1e9f;
    for (int v = 0; v < m_vertices; ++v)
        if (m_class[size_t(v)] == Torso) crotch = std::min(crotch, P[size_t(v) * 3 + 1]);
    out[LegLength] = crotch < 1e8f ? crotch - minY : 0.0f;
    float armLo = 1e9f, armHi = -1e9f, colHi = 0, necLo = 1e9f, necHi = -1e9f, headLo = 1e9f, headHi = -1e9f;
    for (int v = 0; v < m_vertices; ++v) {
        const float x = P[size_t(v) * 3], y = P[size_t(v) * 3 + 1];
        switch (m_class[size_t(v)]) {
        case Arm: armLo = std::min(armLo, x); armHi = std::max(armHi, x); break;
        case Collar: colHi = std::max(colHi, std::fabs(x)); break;
        case Neck: necLo = std::min(necLo, y); necHi = std::max(necHi, y); break;
        case Head: headLo = std::min(headLo, y); headHi = std::max(headHi, y); break;
        default: break;
        }
    }
    out[ArmSpan] = armHi > armLo ? armHi - armLo : 0.0f;
    out[ShoulderWidth] = 2.0f * colHi;
    out[NeckLength] = necHi > necLo ? necHi - necLo : 0.0f;
    out[HeadHeight] = headHi > headLo ? headHi - headLo : 0.0f;
    out[HeadWidth] = extent(0, [](uint8_t c) { return c == Head; }, 0.88f, 1.0f);
    out[ChestWidth] = extent(0, torsoOnly, 0.70f, 0.76f);
    out[WaistWidth] = extent(0, torsoOnly, 0.60f, 0.64f);
    out[HipsWidth] = extent(0, trunk, 0.50f, 0.54f);
    out[ChestDepth] = extent(2, torsoOnly, 0.70f, 0.76f);
    out[BellyDepth] = extent(2, torsoOnly, 0.60f, 0.64f);
    return out;
}

bool Model::build(const Assets& a)
{
    m_valid = false;
    m_vertices = a.mhrVertices;
    m_shapes = a.mhrShapes;
    if (m_vertices <= 0 || m_shapes <= 0 || a.mhrBase.size() != size_t(m_vertices) * 3 || a.skinStart.size() != size_t(m_vertices) + 1 ||
        a.mhrShapeVectors.size() != size_t(m_shapes) * size_t(m_vertices) * 3)
        return false;
    m_base = a.mhrBase;
    m_vectors = a.mhrShapeVectors;
    std::vector<uint8_t> jointClass(size_t(a.jointNames.size()), Torso);
    for (int j = 0; j < a.jointNames.size(); ++j) jointClass[size_t(j)] = classify(a.jointNames[j]);
    m_class.assign(size_t(m_vertices), Torso);
    for (int v = 0; v < m_vertices; ++v) {
        float best = 0.0f;
        for (uint32_t k = a.skinStart[size_t(v)]; k < a.skinStart[size_t(v) + 1]; ++k)
            if (a.skinWeight[k] > best && a.skinJoint[k] >= 0 && size_t(a.skinJoint[k]) < jointClass.size()) { best = a.skinWeight[k]; m_class[size_t(v)] = jointClass[size_t(a.skinJoint[k])]; }
    }
    m_mean = measureShaped(m_base);
    if (m_mean[Height] < 50.0f) return false;
    m_J.assign(size_t(MeasureCount) * size_t(m_shapes), 0.0f);
    std::vector<float> P(m_base.size());
    for (int s = 0; s < m_shapes; ++s) {
        const float* vec = m_vectors.data() + size_t(s) * size_t(m_vertices) * 3;
        for (size_t i = 0; i < P.size(); ++i) P[i] = m_base[i] + vec[i];
        const auto d = measureShaped(P);
        for (int m = 0; m < MeasureCount; ++m) m_J[size_t(m) * size_t(m_shapes) + size_t(s)] = d[size_t(m)] - m_mean[size_t(m)];
    }
    m_valid = true;
    return true;
}

std::array<float, MeasureCount> Model::measure(const std::vector<float>& c) const
{
    if (!m_valid) return {};
    std::vector<float> P(m_base);
    for (int s = 0; s < m_shapes && s < int(c.size()); ++s) {
        if (c[size_t(s)] == 0.0f) continue;
        const float* vec = m_vectors.data() + size_t(s) * size_t(m_vertices) * 3;
        for (size_t i = 0; i < P.size(); ++i) P[i] += c[size_t(s)] * vec[i];
    }
    return measureShaped(P);
}

float Model::offset(Measure m, const std::vector<float>& c) const
{
    if (!m_valid) return 0.0f;
    float v = 0.0f;
    for (int s = 0; s < m_shapes && s < int(c.size()); ++s) v += m_J[size_t(m) * size_t(m_shapes) + size_t(s)] * c[size_t(s)];
    return v;
}

std::vector<float> Model::withOffset(Measure m, float target, const std::vector<float>& coefficients, float limit) const
{
    std::vector<float> out = coefficients;
    out.resize(size_t(std::max(m_shapes, int(coefficients.size()))), 0.0f);
    if (!m_valid) return out;
    const int n = MeasureCount;
    // dc = J^T (J J^T + lambda I)^-1 (d e_m)
    std::vector<double> A(size_t(n) * size_t(n) * 2, 0.0);   // [A | I] for Gauss-Jordan
    double trace = 0.0;
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < n; ++k) {
            double s = 0.0;
            for (int j = 0; j < m_shapes; ++j) s += double(m_J[size_t(i) * size_t(m_shapes) + size_t(j)]) * double(m_J[size_t(k) * size_t(m_shapes) + size_t(j)]);
            A[size_t(i) * size_t(2 * n) + size_t(k)] = s;
            if (i == k) trace += s;
        }
    const double lambda = 1e-3 * trace / double(n) + 1e-9;
    for (int i = 0; i < n; ++i) { A[size_t(i) * size_t(2 * n) + size_t(i)] += lambda; A[size_t(i) * size_t(2 * n) + size_t(n + i)] = 1.0; }
    for (int col = 0; col < n; ++col) {
        int piv = col;
        for (int r = col + 1; r < n; ++r) if (std::fabs(A[size_t(r) * size_t(2 * n) + size_t(col)]) > std::fabs(A[size_t(piv) * size_t(2 * n) + size_t(col)])) piv = r;
        if (std::fabs(A[size_t(piv) * size_t(2 * n) + size_t(col)]) < 1e-12) return out;
        if (piv != col) for (int k = 0; k < 2 * n; ++k) std::swap(A[size_t(piv) * size_t(2 * n) + size_t(k)], A[size_t(col) * size_t(2 * n) + size_t(k)]);
        const double d = A[size_t(col) * size_t(2 * n) + size_t(col)];
        for (int k = 0; k < 2 * n; ++k) A[size_t(col) * size_t(2 * n) + size_t(k)] /= d;
        for (int r = 0; r < n; ++r) {
            if (r == col) continue;
            const double f = A[size_t(r) * size_t(2 * n) + size_t(col)];
            if (f == 0.0) continue;
            for (int k = 0; k < 2 * n; ++k) A[size_t(r) * size_t(2 * n) + size_t(k)] -= f * A[size_t(col) * size_t(2 * n) + size_t(k)];
        }
    }
    const double want = double(target) - double(offset(m, coefficients));
    std::vector<double> y(static_cast<size_t>(n), 0.0);   // column m of the inverse, times want
    for (int i = 0; i < n; ++i) y[size_t(i)] = A[size_t(i) * size_t(2 * n) + size_t(n + int(m))] * want;
    for (int j = 0; j < m_shapes; ++j) {
        double dc = 0.0;
        for (int i = 0; i < n; ++i) dc += double(m_J[size_t(i) * size_t(m_shapes) + size_t(j)]) * y[size_t(i)];
        out[size_t(j)] = std::clamp(out[size_t(j)] + float(dc), -limit, limit);
    }
    return out;
}

namespace {
float spineLengthCm(const Assets& a)
{
    float len = 0.0f;
    for (const char* n : {"c_spine1", "c_spine2", "c_spine3"}) {
        const int j = a.joint(QString::fromLatin1(n));
        if (j < 0 || size_t(j) * 3 + 2 >= a.jointOffsets.size()) continue;
        const float* o = &a.jointOffsets[size_t(j) * 3];
        len += std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
    }
    return len;
}
}  // namespace

BodyFit fitToBody(const Model& model, const Assets& assets, const std::vector<float>& targetCm, const std::vector<float>& coefficients, float maxChange)
{
    BodyFit fit;
    fit.coefficients = coefficients;
    fit.coefficients.resize(size_t(std::max(model.shapes(), int(coefficients.size()))), 0.0f);
    if (!model.valid()) return fit;
    const auto target = model.measurePositions(targetCm);
    if (target[Height] < 50.0f) return fit;
    const auto mean = model.mean();
    // the widths the identity modes can reach: match the target's offset from the mean body
    for (Measure m : {ChestWidth, WaistWidth, HipsWidth, BellyDepth, ChestDepth})
        fit.coefficients = model.withOffset(m, target[size_t(m)] - mean[size_t(m)], fit.coefficients);
    // proportions from the lengths: with the coefficients now settled, what the modes left over
    const auto now = model.measure(fit.coefficients);
    auto ratio = [&](float want, float have) { return have > 1e-3f ? std::clamp(want / have, 1.0f - maxChange, 1.0f + maxChange) : 1.0f; };
    Proportions p;
    p.legs = ratio(target[LegLength], now[LegLength]);
    p.head = ratio(target[HeadHeight], now[HeadHeight]);
    p.shoulders = ratio(target[ShoulderWidth], now[ShoulderWidth]);
    const float armNow = now[ArmSpan] - now[ShoulderWidth], armWant = target[ArmSpan] - target[ShoulderWidth];
    p.arms = ratio(armWant, armNow);
    // torso: whatever of the height the legs, neck and head do not account for; the spine bones carry it
    const float torsoNow = now[Height] - now[LegLength] - now[NeckLength] - now[HeadHeight];
    const float torsoWant = target[Height] - target[LegLength] - target[NeckLength] - target[HeadHeight];
    const float spine = spineLengthCm(assets);
    if (spine > 1.0f) p.torso = std::clamp(1.0f + (torsoWant - torsoNow) / spine, 1.0f - maxChange, 1.0f + maxChange);
    fit.proportions = p;
    fit.ok = true;
    return fit;
}

float rmsDistance(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size() || a.empty()) return -1.0f;
    double sum = 0.0;
    for (size_t i = 0; i + 2 < a.size(); i += 3) {
        const double dx = double(a[i]) - double(b[i]), dy = double(a[i + 1]) - double(b[i + 1]), dz = double(a[i + 2]) - double(b[i + 2]);
        sum += dx * dx + dy * dy + dz * dz;
    }
    return float(std::sqrt(sum / double(a.size() / 3)));
}

}  // namespace BodyShape
}  // namespace CharacterGen
