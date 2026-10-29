#include "KimodoMath.h"

#include <QStringList>

#include <algorithm>
#include <cfenv>
#include <cmath>

namespace AIMotion::KimodoMath {

namespace {

bool isAlnum(QChar c) { return c.isLetterOrNumber(); }

// Python str.capitalize(): first character upper-cased, the rest lower-cased.
QString pyCapitalize(const QString& s)
{
    if (s.isEmpty()) return s;
    return s.left(1).toUpper() + s.mid(1).toLower();
}

} // namespace

QString sanitizeText(const QString& input)
{
    QString text = input.simplified();   // strip + collapse runs of whitespace into one space
    if (text.isEmpty()) return text;

    // Leading non-alphanumerics go. Python's loop leaves i on the LAST index when nothing is alphanumeric.
    int i = 0;
    for (i = 0; i < text.size(); ++i)
        if (isAlnum(text[i])) break;
    if (i == text.size()) i = int(text.size()) - 1;
    text = pyCapitalize(text.mid(i));

    static const QString kFinal = QStringLiteral(".!?\"])'");
    int j = int(text.size()) - 1;
    for (; j >= 0; --j)
        if (isAlnum(text[j]) || kFinal.contains(text[j])) break;
    if (j < 0) j = 0;
    text = text.left(j + 1);
    if (!QStringLiteral(".!?").contains(text.back())) text += QLatin1Char('.');

    for (const QChar brk : {QChar('.'), QChar('!'), QChar('?')}) {
        QStringList parts;
        for (const QString& x : text.split(brk)) {
            const QString y = x.trimmed();
            parts << (y.isEmpty() ? y : y.left(1).toUpper() + y.mid(1));
        }
        text = parts.join(QString(brk) + QLatin1Char(' ')).trimmed();
    }
    return text;
}

LlmPrompt llm2vecPrompt(const QString& sanitizedText)
{
    LlmPrompt p;
    const QString t = sanitizedText.trimmed();
    p.full = QStringLiteral("<|start_header_id|>user<|end_header_id|>\n\n") + t + QStringLiteral("<|eot_id|>");
    p.pooled = t + QStringLiteral("<|eot_id|>");
    return p;
}

DdimSchedule ddimSchedule(const std::vector<float>& base, int steps)
{
    DdimSchedule s;
    const int n = int(base.size());
    if (n < 2 || steps < 1) return s;
    // torch: arange(n) (int64) * python-float stride -> float32, round half-to-even, clamp to n-1.
    const float frac = float(double(n - 1) / double(std::max(1, steps - 1)));
    const int previousMode = std::fegetround();
    std::fesetround(FE_TONEAREST);
    std::vector<float> acp(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int64_t t = std::min<int64_t>(int64_t(std::nearbyint(float(i) * frac)), n - 1);
        if (i < steps) s.timesteps.push_back(t);
        acp[size_t(i)] = base[size_t(t)];
    }
    std::fesetround(previousMode);
    // calc_diffusion_vars: betas from consecutive ratios, then cumprod again (float32 throughout).
    float cum = 1.0f;
    float last = 1.0f;
    s.alphasCumprod.resize(size_t(steps));
    s.alphasCumprodPrev.resize(size_t(steps));
    for (int i = 0; i < n && i < steps; ++i) {
        const float beta = 1.0f - acp[size_t(i)] / last;
        last = acp[size_t(i)];
        const float prev = cum;
        cum *= (1.0f - beta);
        s.alphasCumprodPrev[size_t(i)] = i == 0 ? 1.0f : std::max(prev, 1e-9f);
        s.alphasCumprod[size_t(i)] = std::max(cum, 1e-9f);
    }
    return s;
}

void ddimStep(const DdimSchedule& s, int i, const float* pred, float* x, size_t n)
{
    const float acp = s.alphasCumprod[size_t(i)];
    const float prev = s.alphasCumprodPrev[size_t(i)];
    const float sqrtRecip = 1.0f / std::sqrt(acp);
    const float sqrtRecipM1 = 1.0f / std::sqrt(acp / (1.0f - acp));
    const float a = std::sqrt(prev);
    const float b = std::sqrt(1.0f - prev);
    for (size_t k = 0; k < n; ++k) {
        const float eps = (sqrtRecip * x[k] - pred[k]) / sqrtRecipM1;
        x[k] = pred[k] * a + b * eps;
    }
}

namespace {

// TrajectorySmoother (pos_weight 0, no loop): M = AᵀA + ρI with A the second-difference operator on rows
// 1..n-2. M is symmetric positive definite and pentadiagonal, so a banded LDLᵀ replaces scipy's splu.
class Smoother {
public:
    Smoother(int n, float margin) : m_n(n), m_margin(margin)
    {
        // Band of AᵀA (d0 diagonal, d1/d2 first/second off-diagonals). Row r of A is (-1, 2, -1) at columns
        // r-1, r, r+1, so it adds the outer product of that stencil.
        const auto N = static_cast<size_t>(n);
        std::vector<double> d0(N, 0.0);
        std::vector<double> d1(N, 0.0);
        std::vector<double> d2(N, 0.0);
        for (int r = 1; r <= n - 2; ++r) {
            const auto c = static_cast<size_t>(r);
            d0[c - 1] += 1.0;
            d0[c] += 4.0;
            d0[c + 1] += 1.0;
            d1[c - 1] += -2.0;
            d1[c] += -2.0;
            d2[c - 1] += 1.0;
        }
        double diagMax = 0.0;
        for (double d : d0) diagMax = std::max(diagMax, std::fabs(d));
        m_rho = 0.25 * std::sqrt(diagMax);
        for (double& d : d0) d += m_rho;
        factorize(d0, d1, d2);
    }

    bool valid() const
    {
        return m_n >= 3 && std::all_of(m_D.begin(), m_D.end(), [](double d) { return d > 0.0; });
    }

    void solve(std::vector<double>& r) const
    {
        const auto n = static_cast<size_t>(m_n);
        for (size_t i = 1; i < n; ++i) {
            r[i] -= m_l1[i] * r[i - 1];
            if (i >= 2) r[i] -= m_l2[i] * r[i - 2];
        }
        for (size_t i = 0; i < n; ++i) r[i] /= m_D[i];
        for (size_t i = n - 1; i-- > 0;) {
            r[i] -= m_l1[i + 1] * r[i + 1];
            if (i + 2 < n) r[i] -= m_l2[i + 2] * r[i + 2];
        }
    }

    // ADMM with over-relaxation; x/z/u float32 like the numpy arrays, the linear solve in double.
    Signal smooth(const Signal& target, Signal x) const
    {
        const auto n = static_cast<size_t>(m_n);
        Signal z(n, {0.0f, 0.0f});
        Signal u(n, {0.0f, 0.0f});
        std::vector<double> r0(n);
        std::vector<double> r1(n);
        for (int it = 0; it < 500; ++it) {
            projectOntoMargins(x, u, target, z);
            for (size_t i = 0; i < n; ++i) {
                u[i][0] += kOverRelax * (x[i][0] - z[i][0]);
                u[i][1] += kOverRelax * (x[i][1] - z[i][1]);
                r0[i] = m_rho * double(z[i][0] - u[i][0]);
                r1[i] = m_rho * double(z[i][1] - u[i][1]);
            }
            solve(r0);
            solve(r1);
            for (size_t i = 0; i < n; ++i) x[i] = {float(r0[i]), float(r1[i])};
        }
        return x;
    }

private:
    static constexpr float kOverRelax = 1.8f;

    void factorize(const std::vector<double>& d0, const std::vector<double>& d1, const std::vector<double>& d2)
    {
        const auto n = static_cast<size_t>(m_n);
        m_D.assign(n, 0.0);
        m_l1.assign(n, 0.0);
        m_l2.assign(n, 0.0);
        for (size_t i = 0; i < n; ++i) {
            double d = d0[i];
            if (i >= 1) d -= m_l1[i] * m_l1[i] * m_D[i - 1];
            if (i >= 2) d -= m_l2[i] * m_l2[i] * m_D[i - 2];
            m_D[i] = d;
            if (i + 1 < n) {
                const double carry = i >= 1 ? m_l2[i + 1] * m_l1[i] * m_D[i - 1] : 0.0;
                m_l1[i + 1] = (d1[i] - carry) / d;
            }
            if (i + 2 < n) m_l2[i + 2] = d2[i] / d;
        }
    }

    // z = target + (x + u - target) clamped to the margin radius.
    void projectOntoMargins(const Signal& x, const Signal& u, const Signal& target, Signal& z) const
    {
        for (size_t i = 0; i < x.size(); ++i) {
            float zx = x[i][0] + u[i][0] - target[i][0];
            float zy = x[i][1] + u[i][1] - target[i][1];
            if (const float norm = std::sqrt(zx * zx + zy * zy); double(norm) > double(m_margin)) {
                const auto scale = float(double(m_margin) / double(norm));
                zx *= scale;
                zy *= scale;
            }
            z[i] = {zx + target[i][0], zy + target[i][1]};
        }
    }

    int m_n;
    float m_margin;
    double m_rho = 0.0;
    std::vector<double> m_D;
    std::vector<double> m_l1;
    std::vector<double> m_l2;
};

std::array<float, 2> midpoint(const std::array<float, 2>& a, const std::array<float, 2>& b)
{
    return {(a[0] + b[0]) / 2.0f, (a[1] + b[1]) / 2.0f};
}

// smooth_signal's interleave: the samples between the solved ones become midpoints; the last one extrapolates when
// it has no right neighbour. At step 1 the "interleaved" set is every sample (Python's x[0::1]), so the final pass
// averages each sample with its successor — reproduced as written.
void interleave(Signal& xs, int step)
{
    const int N = int(xs.size());
    const int next = step / 2;
    std::vector<size_t> idx;
    for (int i = 0; i < N; i += step) idx.push_back(size_t(i));
    std::vector<size_t> inter;
    for (int i = next; i < N; i += step) inter.push_back(size_t(i));
    size_t numInter = inter.size();
    const size_t n = idx.size();
    if (numInter == n) {
        const auto& a = xs[idx[n - 1]];
        const auto& b = xs[idx[n - 2]];
        xs[inter.back()] = {a[0] + (a[0] - b[0]) / 2.0f, a[1] + (a[1] - b[1]) / 2.0f};
        --numInter;
    }
    // numpy evaluates the right-hand side (the solved samples, incl. a just-extrapolated tail at step 1) first.
    Signal coarse(n);
    for (size_t k = 0; k < n; ++k) coarse[k] = xs[idx[k]];
    for (size_t k = 0; k < numInter; ++k) xs[inter[k]] = midpoint(coarse[k], coarse[k + 1]);
}

} // namespace

Signal smoothSignal(const Signal& x, float margin)
{
    const int N = int(x.size());
    if (N < 3) return x;
    std::array<float, 2> mean = {0.0f, 0.0f};
    for (const auto& p : x) {
        mean[0] += p[0];
        mean[1] += p[1];
    }
    mean[0] /= float(N);
    mean[1] /= float(N);
    Signal xs(size_t(N), mean);

    int levels = int(std::floor(std::log2(double(N))));
    levels = std::max(levels - 4, 1);
    for (int step = 1 << levels; step >= 1; step /= 2) {
        Signal tgt;
        Signal x0;
        for (int i = 0; i < N; i += step) {
            tgt.push_back(x[size_t(i)]);
            x0.push_back(xs[size_t(i)]);
        }
        const Smoother smoother(int(tgt.size()), margin);
        if (!smoother.valid()) return x;   // Kimodo's splu would raise here (fewer than 3 coarse samples)
        const Signal sm = smoother.smooth(tgt, x0);
        for (size_t k = 0; k < sm.size(); ++k) xs[k * size_t(step)] = sm[k];
        interleave(xs, step);
    }
    return xs;
}

std::array<float, 4> quatFromMatrixWxyz(const float m[9])
{
    const float m00 = m[0];
    const float m01 = m[1];
    const float m02 = m[2];
    const float m10 = m[3];
    const float m11 = m[4];
    const float m12 = m[5];
    const float m20 = m[6];
    const float m21 = m[7];
    const float m22 = m[8];
    const std::array<float, 4> s = {1.0f + m00 + m11 + m22, 1.0f + m00 - m11 - m22, 1.0f - m00 + m11 - m22, 1.0f - m00 - m11 + m22};
    std::array<float, 4> qa{};
    for (size_t k = 0; k < 4; ++k) qa[k] = std::sqrt(s[k] > 0.0f ? s[k] : 0.0f);
    const std::array<std::array<float, 4>, 4> cand = {{
        {qa[0] * qa[0], m21 - m12, m02 - m20, m10 - m01},
        {m21 - m12, qa[1] * qa[1], m10 + m01, m02 + m20},
        {m02 - m20, m10 + m01, qa[2] * qa[2], m12 + m21},
        {m10 - m01, m20 + m02, m21 + m12, qa[3] * qa[3]},
    }};
    size_t best = 0;   // torch.argmax: first maximum
    for (size_t k = 1; k < 4; ++k)
        if (qa[k] > qa[best]) best = k;
    const float denom = 2.0f * std::max(qa[best], 0.1f);
    return {cand[best][0] / denom, cand[best][1] / denom, cand[best][2] / denom, cand[best][3] / denom};
}

void matrixFromQuatWxyz(const std::array<float, 4>& q, float o[9])
{
    const float r = q[0];
    const float i = q[1];
    const float j = q[2];
    const float k = q[3];
    const float two_s = 2.0f / (r * r + i * i + j * j + k * k);
    o[0] = 1 - two_s * (j * j + k * k); o[1] = two_s * (i * j - k * r);     o[2] = two_s * (i * k + j * r);
    o[3] = two_s * (i * j + k * r);     o[4] = 1 - two_s * (i * i + k * k); o[5] = two_s * (j * k - i * r);
    o[6] = two_s * (i * k - j * r);     o[7] = two_s * (j * k + i * r);     o[8] = 1 - two_s * (i * i + j * j);
}

} // namespace AIMotion::KimodoMath
